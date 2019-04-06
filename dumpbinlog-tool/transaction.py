#! /usr/bin/env python3
# -*- coding: utf-8 -*-

from pymysqlreplication import BinLogStreamReader
from pymysqlreplication.row_event import (
    RowsEvent,
    DeleteRowsEvent,
    UpdateRowsEvent,
    WriteRowsEvent,
)
from pymysqlreplication.event import (
    QueryEvent,
    XidEvent,
    RotateEvent,
    GtidEvent,
    XAPrepareEvent
)
from binascii import unhexlify
import re

class Transaction:
    TRX_NORMAL = 'NORMAL'
    TRX_XA_PREPARE = 'XA_PREPARE'
    TRX_XA_COMMIT = 'XA_COMMIT'

    def __init__(self, gtid:str=None, trx_type=TRX_NORMAL, xid:str=None,
                 log_file:str=None, log_pos:int=None):
        self.gtid = gtid
        self.last_committed = None
        self.sequence_number = None
        self.binlog_file = log_file
        self.last_log_pos = log_pos
        self.sql_list = []
        self.XID = xid
        self.type = trx_type
        self.timestamp = 0
        self.is_skip_schema = False

    def isvalid(self) -> bool:
        return self.gtid and self.last_committed is not None and \
            self.sequence_number is not None and self.binlog_file

    def dump(self):
        print('GTID:', self.gtid,
              'last_committed:', self.last_committed,
              'sequence_number:', self.sequence_number,
              '\nfile:', self.binlog_file, 'pos:', self.last_log_pos,
              '\ntype:', self.type)
        if self.is_skip_schema:
            print('Some SQL in this trx is skipped')
        print('SQL:')
        for sql in self.sql_list:
            print(sql)
        print()

    def brief(self) -> str:
        return '..'.join(sql.split(' ')[0] for sql in self.sql_list)

    def interleaved(self, other: 'Transaction') -> bool:
        assert self.last_committed < self.sequence_number
        assert other.last_committed < other.sequence_number
        if self.last_committed < other.last_committed and \
           self.sequence_number > other.last_committed:
            return True
        elif other.last_committed < self.sequence_number and \
             other.sequence_number > self.last_committed:
            return True
        else:
            return False

    def __repr__(self) -> str:
        return '{} {} {} {} {} {}'.format(self.gtid, self.type, self.XID,
                                          self.binlog_file, self.last_log_pos,
                                          self.timestamp)


def qstr(obj) -> str:
    return "'{}'".format(str(obj))

def sql_delete(table, row) -> str:
    sql = "delete from {} where ".format(table)
    sql += ' and '.join([str(k)+'='+qstr(v) for k, v in row.items()])
    return sql

def sql_update(table, before_row, after_row) -> str:
    sql = 'update {} set '.format(table)
    ct = 0
    l = len(after_row.items())
    for k, v in after_row.items():
        ct += 1
        if v is None:
            sql += (str(k) + '=' + 'NULL')
        else:
            sql += (str(k) + '=' + qstr(v))
        if ct != l:
            sql += ','
    sql += ' where '
    sql += ' and '.join([str(k)+'='+qstr(v) for k, v in before_row.items()])
    return sql

def sql_insert(table, row) -> str:
    sql = 'insert into {}('.format(table)
    keys = row.keys()
    sql += ','.join([str(k) for k in keys])
    sql += ') values('
    ct = 0
    l = len(keys)
    for k in keys:
        ct+= 1
        if row[k] is None:
            sql +="NULL"
        else:
            sql += qstr(row[k])
        if ct != l:
            sql += ','
    sql += ')'
    return sql

def is_ddl(sql: str) -> bool:
    ddl_pattern = ['create table', 'drop table', 'create index', 'drop index',
     'truncate table', 'alter table', 'alter index', 'create database', 'drop database', 'create user', 'drop user']
    no_comment = re.sub('/\*.*?\*/', '', sql, flags=re.S)
    formatted = ' '.join(no_comment.lower().split())
    return any(formatted.startswith(x) for x in ddl_pattern)

def is_ddl_database(sql: str) -> bool:
    ddl_pattern = ['create database', 'drop database']
    no_comment = re.sub('/\*.*?\*/', '', sql, flags=re.S)
    formatted = ' '.join(no_comment.lower().split())
    return any(formatted.startswith(x) for x in ddl_pattern)


class BinlogTrxReader:

    def __init__(self, config,
                 server_id,
                 blocking,
                 resume_stream,
                 log_file=None,
                 log_pos=None,
                 auto_position=None):

        self.event_stream = BinLogStreamReader(
            connection_settings=config.BINLOG_MYSQL,
            server_id=server_id,
            blocking=blocking,
            resume_stream=resume_stream,
            log_file=log_file,
            log_pos=log_pos,
            auto_position=auto_position
        )
        self._SKIP_SCHEMAS = config.SKIP_SCHEMAS
        self._ALLOW_TABLES = config.ALLOW_TABLES
        self._IGNORE_DDL = config.IGNORE_DDL

    def __iter__(self):
        return iter(self.fetch_one, None)

    def _get_xid(self, event:QueryEvent) -> str:
        sql = event.query
        assert sql.lower().startswith('xa')
        all_id = sql.split(' ')[2]
        hex_id = all_id.split(',')[0]
        return unhexlify(hex_id[2:-1]).decode()

    def fetch_one(self) -> Transaction:
        sql_events = [DeleteRowsEvent, WriteRowsEvent,
                      UpdateRowsEvent, QueryEvent, XidEvent,
                      XAPrepareEvent]

        trx = Transaction()
        for event in self.event_stream:
            if isinstance(event, RotateEvent):
                self.current_file = event.next_binlog
            elif isinstance(event, GtidEvent):
                trx.timestamp = event.timestamp
                trx.gtid = event.gtid
                trx.last_committed = event.last_committed
                trx.sequence_number = event.sequence_number
                trx.binlog_file = self.current_file
            else:
                finished = self._feed_event(trx, event)
                if finished:
                    trx.last_log_pos = event.packet.log_pos
                    self._trim(trx)
                    return trx

    def _process_rows_event(self, trx: Transaction, event: RowsEvent):
        if self._SKIP_SCHEMAS and event.schema in self._SKIP_SCHEMAS:
            trx.is_skip_schema = True
            return
        if self._ALLOW_TABLES and (event.schema, event.table) not in self._ALLOW_TABLES:
            return

        table = "%s.%s" % (event.schema, event.table)
        if isinstance(event, DeleteRowsEvent):
            trx.sql_list += [sql_delete(table, row['values']) for row in event.rows]
        elif isinstance(event, UpdateRowsEvent):
            trx.sql_list += [sql_update(table, row["before_values"], row["after_values"])\
                             for row in event.rows]
        elif isinstance(event, WriteRowsEvent):
            trx.sql_list += [sql_insert(table, row['values']) for row in event.rows]

    def _feed_event(self, trx: Transaction, event) -> bool:
        '''return: is this transaction finished
        '''
        if isinstance(event, RowsEvent):
            self._process_rows_event(trx, event)
            return False
        elif isinstance(event, XidEvent):
            trx.sql_list.append('commit')
            assert trx.isvalid()
            return True
        elif isinstance(event, XAPrepareEvent):
            if event.one_phase:
                trx.sql_list.append('commit')
            else:
                trx.type = trx.TRX_XA_PREPARE
                trx.XID = event.xid
            assert trx.isvalid()
            return True
        elif isinstance(event, QueryEvent):
            sql = event.query
            if sql.startswith('XA START'):
                trx.sql_list.append('START TRANSACTION')
            elif sql.startswith('XA ROLLBACK'):
                trx.sql_list.append('ROLLBACK')
            elif sql.startswith('XA END'):
                pass
            elif sql.startswith('XA COMMIT'):
                trx.sql_list.append('COMMIT')
                trx.type = Transaction.TRX_XA_COMMIT
                trx.XID = self._get_xid(event)
                assert trx.isvalid()
                return True
            elif is_ddl(sql):
                if self._IGNORE_DDL:
                    return True
                if event.schema_length and not is_ddl_database(sql):
                    trx.sql_list.append('use '+event.schema.decode())
                trx.sql_list.append(sql)
                assert trx.isvalid()
                return True
            else:
                trx.sql_list.append(sql)
                return False

    def _trim(self, trx: Transaction):
        if trx.type == Transaction.TRX_NORMAL:
            if len(trx.sql_list) == 3 \
               and trx.sql_list[0].lower() == 'begin' \
               and trx.sql_list[2].lower() == 'commit':
                trx.sql_list = trx.sql_list[1:2]
            if len(trx.sql_list) == 2 \
               and trx.sql_list[0].lower() == 'begin' \
               and trx.sql_list[1].lower() == 'commit':
                trx.sql_list = []
        elif trx.type == Transaction.TRX_XA_PREPARE:
            if len(trx.sql_list) == 1 \
               and trx.sql_list[0].lower() == 'start transaction':
                trx.sql_list = []
        else:
            pass

    def close(self):
        self.event_stream.close()
