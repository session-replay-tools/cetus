#! /usr/bin/env python3
# -*- coding: utf-8 -*-

import logging

main_formatter = logging.Formatter('%(asctime)s %(levelname)s %(message)s')
trx_formatter = logging.Formatter('%(asctime)s %(message)s')

def _setup_logger(name, log_file, formatter, level=logging.INFO):
    """Function setup as many loggers as you want"""
    import os
    print('logger work dir:', os.getcwd())
    handler = logging.FileHandler(log_file)        
    handler.setFormatter(formatter)

    logger = logging.getLogger(name)
    logger.setLevel(level)
    logger.addHandler(handler)

    return logger

def init_logger(level):
    _setup_logger('trx', 'progress.log', trx_formatter)
    _setup_logger('main', 'sqldump.log', main_formatter, level=level)

# use with logging.getLogger(name)
