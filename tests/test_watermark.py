from swsscommon import swsscommon
import os
import re
import time
import json
import pytest
import redis


class SaiWmStats:
    queue_shared = "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES"
    pg_shared = "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES"
    pg_headroom = "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES"


class WmTables:
    persistent = "PERSISTENT_WATERMARKS"
    periodic = "PERIODIC_WATERMARKS"
    user = "USER_WATERMARKS"


class TestWatermark(object):

    DEFAULT_TELEMETRY_INTERVAL = 120
    NEW_INTERVAL = 5
    DEFAULT_POLL_INTERVAL = 10

    def enable_unittests(self, dvs, status):
        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)    
        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", status, fvp)

    def set_counter(self, dvs, obj_type, obj_id, attr, val):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")

        r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.ASIC_DB)
        rid = r.hget("VIDTORID", obj_id)

        assert rid is not None

        fvp = swsscommon.FieldValuePairs([(attr, val)])
        key = rid

        ntf.send("set_stats", key, fvp)

    def populate_asic(self, dvs, obj_type, attr, val):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        oids = self.qs if obj_type == "SAI_OBJECT_TYPE_QUEUE" else self.pgs

        for obj_id in oids:
            self.set_counter(dvs, obj_type, obj_id, attr, val)
    
    def populate_asic_all(self, dvs, val):
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_QUEUE", SaiWmStats.queue_shared, val)
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", SaiWmStats.pg_shared, val)
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", SaiWmStats.pg_headroom, val)
        time.sleep(self.DEFAULT_POLL_INTERVAL)

    def verify_value(self, dvs, obj_ids, table_name, watermark_name, expected_value):

        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        table = swsscommon.Table(counters_db, table_name)
        
        for obj_id in obj_ids:

            ret = table.get(obj_id)

            status = ret[0]
            assert status
            keyvalues = ret[1]
            found = False
            for key, value in keyvalues:
              if key == watermark_name:
                  assert value == expected_value
                  found = True
            assert found, "no such watermark found"

    def get_oids(self, dvs, obj_type):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "ASIC_STATE:{0}".format(obj_type))
        keys = tbl.getKeys()
        return keys

    def set_up_flex_counter(self, dvs):
        for q in self.qs:
            dvs.runcmd("redis-cli -n 5 hset 'FLEX_COUNTER_TABLE:QUEUE_WATERMARK_STAT_COUNTER:{}' ".format(q) + \
                      "QUEUE_COUNTER_ID_LIST SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES")

        for pg in self.pgs:
            dvs.runcmd("redis-cli -n 5 hset 'FLEX_COUNTER_TABLE:PG_WATERMARK_STAT_COUNTER:{}' ".format(pg) + \
                      "PG_COUNTER_ID_LIST 'SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES,SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES'")

        dvs.runcmd("redis-cli -n 4 hset 'FLEX_COUNTER_TABLE|PG_WATERMARK' 'FLEX_COUNTER_STATUS' 'enable'")
        dvs.runcmd("redis-cli -n 4 hset 'FLEX_COUNTER_TABLE|QUEUE_WATERMARK' 'FLEX_COUNTER_STATUS' 'enable'")

        self.populate_asic(dvs, "SAI_OBJECT_TYPE_QUEUE", SaiWmStats.queue_shared, "0")
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", SaiWmStats.pg_shared, "0")
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", SaiWmStats.pg_headroom, "0")

        time.sleep(self.DEFAULT_TELEMETRY_INTERVAL*2)

    def set_up(self, dvs):
        
        self.qs = self.get_oids(dvs, "SAI_OBJECT_TYPE_QUEUE")
        self.pgs = self.get_oids(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")  

        db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "COUNTERS_QUEUE_TYPE_MAP")                

        self.uc_q = []
        self.mc_q = []

        for q in self.qs:
             if self.qs.index(q) % 16 < 8:
                 tbl.set('', [(q, "SAI_QUEUE_TYPE_UNICAST")])
                 self.uc_q.append(q)
             else:
                 tbl.set('', [(q, "SAI_QUEUE_TYPE_MULTICAST")])
                 self.mc_q.append(q)

    def test_telemetry_period(self, dvs):
        
        self.set_up(dvs)
        self.set_up_flex_counter(dvs)
        self.enable_unittests(dvs, "true")

        self.populate_asic_all(dvs, "100")

        time.sleep(self.DEFAULT_TELEMETRY_INTERVAL + 1)

        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_shared, "0")
        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_headroom, "0")
        self.verify_value(dvs, self.qs, WmTables.periodic, SaiWmStats.queue_shared, "0")

        self.populate_asic_all(dvs, "123")

        dvs.runcmd("config watermark telemetry interval {}".format(5))

        time.sleep(self.DEFAULT_TELEMETRY_INTERVAL + 1)
        time.sleep(self.NEW_INTERVAL + 1)

        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_shared, "0")
        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_headroom, "0")
        self.verify_value(dvs, self.qs, WmTables.periodic, SaiWmStats.queue_shared, "0")
     
        self.enable_unittests(dvs, "false")

    @pytest.mark.skip(reason="This test is not stable enough")
    def test_lua_plugins(self, dvs):
        
        self.set_up(dvs)
        self.set_up_flex_counter(dvs)
        self.enable_unittests(dvs, "true")

        self.populate_asic_all(dvs, "192")

        for table_name in [WmTables.user, WmTables.persistent]:
            self.verify_value(dvs, self.qs, table_name, SaiWmStats.queue_shared, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_headroom, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_shared, "192")
        
        self.populate_asic_all(dvs, "96")

        for table_name in [WmTables.user, WmTables.persistent]:
            self.verify_value(dvs, self.qs, table_name, SaiWmStats.queue_shared, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_headroom, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_shared, "192")

        self.populate_asic_all(dvs, "288")
        
        for table_name in [WmTables.user, WmTables.persistent]:
            self.verify_value(dvs, self.qs, table_name, SaiWmStats.queue_shared, "288")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_headroom, "288")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_shared, "288")

        self.enable_unittests(dvs, "false")

    @pytest.mark.skip(reason="This test is not stable enough")
    def test_clear(self, dvs):

        self.set_up(dvs)
        self.enable_unittests(dvs, "true")

        self.populate_asic_all(dvs, "288")

        # clear pg shared watermark, and verify that headroom watermark and persistent watermarks are not affected

        exitcode, output = dvs.runcmd("sonic-clear priority-group watermark shared")
        time.sleep(1)
        assert exitcode == 0, "CLI failure: %s" % output
        # make sure it cleared
        self.verify_value(dvs, self.pgs, WmTables.user, SaiWmStats.pg_shared, "0")

        # make sure the rest is untouched

        self.verify_value(dvs, self.pgs, WmTables.user, SaiWmStats.pg_headroom, "288") 
        self.verify_value(dvs, self.pgs, WmTables.persistent, SaiWmStats.pg_shared, "288") 
        self.verify_value(dvs, self.pgs, WmTables.persistent, SaiWmStats.pg_headroom, "288") 

        # clear queue unicast persistent watermark, and verify that multicast watermark and user watermarks are not affected

        exitcode, output = dvs.runcmd("sonic-clear queue persistent-watermark unicast")
        time.sleep(1)
        assert exitcode == 0, "CLI failure: %s" % output

        # make sure it cleared
        self.verify_value(dvs, self.uc_q, WmTables.persistent, SaiWmStats.queue_shared, "0")

        # make sure the rest is untouched

        self.verify_value(dvs, self.mc_q, WmTables.persistent, SaiWmStats.queue_shared, "288") 
        self.verify_value(dvs, self.uc_q, WmTables.user, SaiWmStats.queue_shared, "288") 
        self.verify_value(dvs, self.mc_q, WmTables.user, SaiWmStats.queue_shared, "288") 

        self.enable_unittests(dvs, "false")
