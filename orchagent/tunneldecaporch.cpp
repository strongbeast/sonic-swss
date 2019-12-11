#include <string.h>
#include <inttypes.h>
#include "tunneldecaporch.h"
#include "portsorch.h"
#include "logger.h"
#include "swssnet.h"

extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_router_interface_api_t* sai_router_intfs_api;

extern sai_object_id_t  gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;

TunnelDecapOrch::TunnelDecapOrch(DBConnector *db, string tableName) : Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

void TunnelDecapOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        IpAddresses ip_addresses;
        IpAddress src_ip;
        IpAddress* p_src_ip = nullptr;
        string tunnel_type;
        string dscp_mode;
        string ecn_mode;
        string ttl_mode;
        bool valid = true;

        // checking to see if the tunnel already exists
        bool exists = (tunnelTable.find(key) != tunnelTable.end());

        if (op == SET_COMMAND)
        {

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "tunnel_type")
                {
                    tunnel_type = fvValue(i);
                    if (tunnel_type != "IPINIP")
                    {
                        SWSS_LOG_ERROR("Invalid tunnel type %s", tunnel_type.c_str());
                        valid = false;
                        break;
                    }
                }
                else if (fvField(i) == "dst_ip")
                {
                    try
                    {
                        ip_addresses = IpAddresses(fvValue(i));
                    }
                    catch (const std::invalid_argument &e)
                    {
                        SWSS_LOG_ERROR("%s", e.what());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        setIpAttribute(key, ip_addresses, tunnelTable.find(key)->second.tunnel_id);
                    }
                }
                else if (fvField(i) == "src_ip")
                {
                    try
                    {
                        src_ip = IpAddress(fvValue(i));
                        p_src_ip = &src_ip;
                    }
                    catch (const std::invalid_argument &e)
                    {
                        SWSS_LOG_ERROR("%s", e.what());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        SWSS_LOG_ERROR("cannot modify src ip for existing tunnel");
                    }
                }
                else if (fvField(i) == "dscp_mode")
                {
                    dscp_mode = fvValue(i);
                    if (dscp_mode != "uniform" && dscp_mode != "pipe")
                    {
                        SWSS_LOG_ERROR("Invalid dscp mode %s\n", dscp_mode.c_str());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        setTunnelAttribute(fvField(i), dscp_mode, tunnelTable.find(key)->second.tunnel_id);
                    }
                }
                else if (fvField(i) == "ecn_mode")
                {
                    ecn_mode = fvValue(i);
                    if (ecn_mode != "copy_from_outer" && ecn_mode != "standard")
                    {
                        SWSS_LOG_ERROR("Invalid ecn mode %s\n", ecn_mode.c_str());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        setTunnelAttribute(fvField(i), ecn_mode, tunnelTable.find(key)->second.tunnel_id);
                    }
                }
                else if (fvField(i) == "ttl_mode")
                {
                    ttl_mode = fvValue(i);
                    if (ttl_mode != "uniform" && ttl_mode != "pipe")
                    {
                        SWSS_LOG_ERROR("Invalid ttl mode %s\n", ttl_mode.c_str());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        setTunnelAttribute(fvField(i), ttl_mode, tunnelTable.find(key)->second.tunnel_id);
                    }
                }
            }

            // create new tunnel if it doesn't exists already
            if (valid && !exists)
            {
                if (addDecapTunnel(key, tunnel_type, ip_addresses, p_src_ip, dscp_mode, ecn_mode, ttl_mode))
                {
                    SWSS_LOG_NOTICE("Tunnel(s) added to ASIC_DB.");
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to add tunnels to ASIC_DB.");
                }
            }
        }

        if (op == DEL_COMMAND)
        {
            if (exists)
            {
                removeDecapTunnel(key);
            }
            else
            {
                SWSS_LOG_ERROR("Tunnel cannot be removed since it doesn't exist.");
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

/**
 * Function Description:
 *    @brief adds a decap tunnel to ASIC_DB
 *
 * Arguments:
 *    @param[in] type - type of tunnel
 *    @param[in] dst_ip - destination ip address to decap
 *    @param[in] p_src_ip - source ip address for encap (nullptr to skip this)
 *    @param[in] dscp - dscp mode (uniform/pipe)
 *    @param[in] ecn - ecn mode (copy_from_outer/standard)
 *    @param[in] ttl - ttl mode (uniform/pipe)
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::addDecapTunnel(string key, string type, IpAddresses dst_ip, IpAddress* p_src_ip, string dscp, string ecn, string ttl)
{

    SWSS_LOG_ENTER();

    sai_status_t status;

    // adding tunnel attributes to array and writing to ASIC_DB
    sai_attribute_t attr;
    vector<sai_attribute_t> tunnel_attrs;
    sai_object_id_t overlayIfId;

    // create the overlay router interface to create a LOOPBACK type router interface (decap)
    vector<sai_attribute_t> overlay_intf_attrs;

    sai_attribute_t overlay_intf_attr;
    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    overlay_intf_attr.value.oid = gVirtualRouterId;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    overlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    status = sai_router_intfs_api->create_router_interface(&overlayIfId, gSwitchId, (uint32_t)overlay_intf_attrs.size(), overlay_intf_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create overlay router interface %d", status);
        return false;
    }

    SWSS_LOG_NOTICE("Create overlay loopback router interface oid:%" PRIx64, overlayIfId);

    // tunnel type (only ipinip for now)
    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    attr.value.oid = overlayIfId;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(attr);

    // tunnel src ip
    if (p_src_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
        copy(attr.value.ipaddr, p_src_ip->to_string());
        tunnel_attrs.push_back(attr);
    }

    // decap ecn mode (copy from outer/standard)
    attr.id = SAI_TUNNEL_ATTR_DECAP_ECN_MODE;
    if (ecn == "copy_from_outer")
    {
        attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER;
    }
    else if (ecn == "standard")
    {
        attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_STANDARD;
    }
    tunnel_attrs.push_back(attr);

    // ttl mode (uniform/pipe)
    attr.id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
    if (ttl == "uniform")
    {
        attr.value.s32 = SAI_TUNNEL_TTL_MODE_UNIFORM_MODEL;
    }
    else if (ttl == "pipe")
    {
        attr.value.s32 = SAI_TUNNEL_TTL_MODE_PIPE_MODEL;
    }
    tunnel_attrs.push_back(attr);

    // dscp mode (uniform/pipe)
    attr.id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
    if (dscp == "uniform")
    {
        attr.value.s32 = SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL;
    }
    else if (dscp == "pipe")
    {
        attr.value.s32 = SAI_TUNNEL_DSCP_MODE_PIPE_MODEL;
    }
    tunnel_attrs.push_back(attr);

    // write attributes to ASIC_DB
    sai_object_id_t tunnel_id;
    status = sai_tunnel_api->create_tunnel(&tunnel_id, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel");
        return false;
    }

    tunnelTable[key] = { tunnel_id, overlayIfId, {} };

    // TODO:
    // there should also be "business logic" for netbouncer in the "tunnel application" code, which is a different source file and daemon process

    // create a decap tunnel entry for every ip
    if (!addDecapTunnelTermEntries(key, dst_ip, tunnel_id))
    {
        return false;
    }

    return true;
}

/**
 * Function Description:
 *    @brief adds a decap tunnel termination entry to ASIC_DB
 *
 * Arguments:
 *    @param[in] tunnelKey - key of the tunnel from APP_DB
 *    @param[in] dst_ip - destination ip addresses to decap
 *    @param[in] tunnel_id - the id of the tunnel
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::addDecapTunnelTermEntries(string tunnelKey, IpAddresses dst_ip, sai_object_id_t tunnel_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    // adding tunnel table entry attributes to array and writing to ASIC_DB
    vector<sai_attribute_t> tunnel_table_entry_attrs;
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = gVirtualRouterId;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel_id;
    tunnel_table_entry_attrs.push_back(attr);

    TunnelEntry *tunnel_info = &tunnelTable.find(tunnelKey)->second;

    // loop through the IP list and create a new tunnel table entry for every IP (in network byte order)
    set<IpAddress> tunnel_ips = dst_ip.getIpAddresses();
    for (auto it = tunnel_ips.begin(); it != tunnel_ips.end(); ++it)
    {
        const IpAddress& ia = *it;
        string ip = ia.to_string();

        // check if the there's an entry already for the ip
        if (existingIps.find(ip) != existingIps.end())
        {
            SWSS_LOG_ERROR("%s already exists. Did not create entry.", ip.c_str());
        }
        else
        {
            attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
            copy(attr.value.ipaddr, ia);
            tunnel_table_entry_attrs.push_back(attr);

            // create the tunnel table entry
            sai_object_id_t tunnel_term_table_entry_id;
            sai_status_t status = sai_tunnel_api->create_tunnel_term_table_entry(&tunnel_term_table_entry_id, gSwitchId, (uint32_t)tunnel_table_entry_attrs.size(), tunnel_table_entry_attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create tunnel entry table for ip: %s", ip.c_str());
                return false;
            }

            // insert into ip to entry mapping
            existingIps.insert(ip);

            // insert entry id and ip into tunnel mapping
            tunnel_info->tunnel_term_info.push_back({ tunnel_term_table_entry_id, ip });

            // pop the last element for the next loop
            tunnel_table_entry_attrs.pop_back();

            SWSS_LOG_NOTICE("Created tunnel entry for ip: %s", ip.c_str());
        }

    }
    return true;
}

/**
 * Function Description:
 *    @brief sets attributes for a tunnel
 *
 * Arguments:
 *    @param[in] field - field to set the attribute for
 *    @param[in] value - value to set the attribute to
 *    @param[in] existing_tunnel_id - the id of the tunnel you want to set the attribute for
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::setTunnelAttribute(string field, string value, sai_object_id_t existing_tunnel_id)
{

    sai_attribute_t attr;

    if (field == "ecn_mode")
    {
        // decap ecn mode (copy from outer/standard)
        attr.id = SAI_TUNNEL_ATTR_DECAP_ECN_MODE;
        if (value == "copy_from_outer")
        {
            attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER;
        }
        else if (value == "standard")
        {
            attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_STANDARD;
        }
    }

    if (field == "ttl_mode")
    {
        // ttl mode (uniform/pipe)
        attr.id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
        if (value == "uniform")
        {
            attr.value.s32 = SAI_TUNNEL_TTL_MODE_UNIFORM_MODEL;
        }
        else if (value == "pipe")
        {
            attr.value.s32 = SAI_TUNNEL_TTL_MODE_PIPE_MODEL;
        }
    }

    if (field == "dscp_mode")
    {
        // dscp mode (uniform/pipe)
        attr.id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
        if (value == "uniform")
        {
            attr.value.s32 = SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL;
        }
        else if (value == "pipe")
        {
            attr.value.s32 = SAI_TUNNEL_DSCP_MODE_PIPE_MODEL;
        }
    }

    sai_status_t status = sai_tunnel_api->set_tunnel_attribute(existing_tunnel_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set attribute %s with value %s\n", field.c_str(), value.c_str());
        return false;
    }
    SWSS_LOG_NOTICE("Set attribute %s with value %s\n", field.c_str(), value.c_str());
    return true;
}

/**
 * Function Description:
 *    @brief sets ips for a particular tunnel. deletes ips that are old and adds new ones
 *
 * Arguments:
 *    @param[in] key - key of the tunnel from APP_DB
 *    @param[in] new_ip_addresses - new destination ip addresses to decap (comes from APP_DB)
 *    @param[in] tunnel_id - the id of the tunnel
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::setIpAttribute(string key, IpAddresses new_ip_addresses, sai_object_id_t tunnel_id)
{
    TunnelEntry *tunnel_info = &tunnelTable.find(key)->second;

    // make a copy of tunnel_term_info to loop through
    vector<TunnelTermEntry> tunnel_term_info_copy(tunnel_info->tunnel_term_info);

    tunnel_info->tunnel_term_info.clear();

    // loop through original ips and remove ips not in the new ip_addresses
    for (auto it = tunnel_term_info_copy.begin(); it != tunnel_term_info_copy.end(); ++it)
    {
        TunnelTermEntry tunnel_entry_info = *it;
        string ip = tunnel_entry_info.ip_address;
        if (!new_ip_addresses.contains(ip))
        {
            if (!removeDecapTunnelTermEntry(tunnel_entry_info.tunnel_term_id, ip))
            {
                return false;
            }
        }
        else
        {
            // add the data into the tunnel_term_info
            tunnel_info->tunnel_term_info.push_back({ tunnel_entry_info.tunnel_term_id, ip });
        }
    }

    // add all the new ip addresses
    if(!addDecapTunnelTermEntries(key, new_ip_addresses, tunnel_id))
    {
        return false;
    }

    return true;
}

/**
 * Function Description:
 *    @brief remove decap tunnel
 *
 * Arguments:
 *    @param[in] key - key of the tunnel from APP_DB
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::removeDecapTunnel(string key)
{
    sai_status_t status;
    TunnelEntry *tunnel_info = &tunnelTable.find(key)->second;

    // loop through the tunnel entry ids related to the tunnel and remove them before removing the tunnel
    for (auto it = tunnel_info->tunnel_term_info.begin(); it != tunnel_info->tunnel_term_info.end(); ++it)
    {
        TunnelTermEntry tunnel_entry_info = *it;
        if (!removeDecapTunnelTermEntry(tunnel_entry_info.tunnel_term_id, tunnel_entry_info.ip_address))
        {
            return false;
        }
    }

    tunnel_info->tunnel_term_info = {};

    status = sai_tunnel_api->remove_tunnel(tunnel_info->tunnel_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel: %" PRIu64, tunnel_info->tunnel_id);
        return false;
    }

    // delete overlay loopback interface
    status = sai_router_intfs_api->remove_router_interface(tunnel_info->overlay_intf_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel overlay interface: %" PRIu64, tunnel_info->overlay_intf_id);
        return false;
    }

    tunnelTable.erase(key);
    return true;
}

/**
 * Function Description:
 *    @brief remove decap tunnel termination entry
 *
 * Arguments:
 *    @param[in] key - key of the tunnel from APP_DB
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::removeDecapTunnelTermEntry(sai_object_id_t tunnel_term_id, string ip)
{
    sai_status_t status;

    status = sai_tunnel_api->remove_tunnel_term_table_entry(tunnel_term_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel table entry: %" PRIu64, tunnel_term_id);
        return false;
    }

    // making sure to remove all instances of the ip address
    existingIps.erase(ip);
    SWSS_LOG_NOTICE("Removed decap tunnel term entry with ip address: %s", ip.c_str());
    return true;
}
