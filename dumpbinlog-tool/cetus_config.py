#! /usr/bin/env python3
# -*- coding: utf-8 -*-
import os
import json
import configparser
import logging

class CetusConf:
    def __init__(self, sharding_conf_file):
        with open(os.path.expanduser(sharding_conf_file)) as f:
            self.shard_config = json.load(f)

    def sharded_and_single_tables(self):
        return self.sharded_tables() + self.single_tables()

    def sharded_tables(self):
        return [(t['db'], t['table']) for t in self.shard_config['table']]

    def single_tables(self):
        return [(t['db'], t['table']) for t in self.shard_config['single_tables']]

class BinlogConfig:
    BINLOG_MYSQL = {}
    OUTPUT_MYSQL = {}
    SKIP_SCHEMAS = None
    BINLOG_POS = {}
    IGNORE_DDL = False
    LOG_LEVEL = logging.INFO
    ONLY_SHARDING_TABLE = None
    ALLOW_TABLES = None # get these table from file: ONLY_SHARDING_TABLE

    def __init__(self, filename):
        conf = configparser.ConfigParser()
        conf.read(filename)
        self.BINLOG_MYSQL['host'] = conf['BINLOG_MYSQL']['host']
        self.BINLOG_MYSQL['port'] = int(conf['BINLOG_MYSQL']['port'])
        self.BINLOG_MYSQL['user'] = conf['BINLOG_MYSQL']['user']
        self.BINLOG_MYSQL['passwd'] = conf['BINLOG_MYSQL']['password']

        self.OUTPUT_MYSQL['host'] = conf['OUTPUT_MYSQL']['host']
        self.OUTPUT_MYSQL['port'] = int(conf['OUTPUT_MYSQL']['port'])
        self.OUTPUT_MYSQL['user'] = conf['OUTPUT_MYSQL']['user']
        self.OUTPUT_MYSQL['password'] = conf['OUTPUT_MYSQL']['password']

        skip_schemas = conf['DEFAULT'].get('skip_schemas', None)
        if skip_schemas:
            self.SKIP_SCHEMAS = [s.strip() for s in skip_schemas.split(',')]

        sharding_file = conf['DEFAULT'].get('only_sharding_table', None)
        if sharding_file:
            self.ONLY_SHARDING_TABLE = sharding_file

        pos = conf['DEFAULT'].get('auto_position', None)
        if pos:
            self.BINLOG_POS['auto_position'] = pos
        log_file = conf['DEFAULT'].get('log_file', None)
        if log_file:
            self.BINLOG_POS['log_file'] = log_file
        log_pos = conf['DEFAULT'].get('log_pos', None)
        if log_file:
            self.BINLOG_POS['log_pos'] = int(log_pos)

        level = conf['DEFAULT'].get('log_level', 'INFO')
        if level == 'DEBUG':
            self.LOG_LEVEL = logging.DEBUG

        if conf['DEFAULT'].getboolean('ignore_ddl', False):
            self.IGNORE_DDL = True

    def dump(self):
        print('  SKIP_SCHEMAS:', self.SKIP_SCHEMAS)
        print('  BINLOG_POS:', self.BINLOG_POS)
        print('  IGNORE_DDL:', self.IGNORE_DDL)
        print('  LOG_LEVEL:', self.LOG_LEVEL)
        print('  ONLY_SHARDING_TABLE:', self.ONLY_SHARDING_TABLE)
        print('')

