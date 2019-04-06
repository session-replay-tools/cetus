#!/usr/bin/env python3
# -*- coding: utf-8 -*-
from executor import TransactionDispatcher
from transaction import Transaction, BinlogTrxReader
import cetus_config
import logging
import queue
import sys
import os
import recovery
import logger
import argparse

transaction_queue = queue.Queue()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-d', dest='base_dir', type=str, default=os.getcwd(), help='Set working directory')
    parser.add_argument('-n', dest='dry_run', action='store_true', help='Not output to MySQL, just print')
    args = parser.parse_args()

    os.chdir(args.base_dir)
    print("Set working dir: ", args.base_dir)

    config = cetus_config.BinlogConfig('binlog.conf')
    config.dump()

    logger.init_logger(config.LOG_LEVEL)
    main_logger = logging.getLogger('main')

    main_logger.info('APP START')
    if config.ONLY_SHARDING_TABLE:
        conf = cetus_config.CetusConf(config.ONLY_SHARDING_TABLE)
        config.ALLOW_TABLES = conf.sharded_and_single_tables()

    prev_execution = None
    pos_config = config.BINLOG_POS

    if os.path.exists('progress.log'):
        prev_execution = recovery.read('progress.log')

    if prev_execution is not None:
        print('Found previous execution log, recover from it.')
        main_logger.info('Found previous execution log, recover from it.')
        #pos_config = {'auto_position': prev_execution.gtid_executed}
        pos_config = {'log_file': prev_execution.start_log_file,
                      'log_pos': prev_execution.start_log_pos}

    stream = BinlogTrxReader(
        config=config,
        server_id=100,
        blocking=True,
        resume_stream=True,
        **pos_config
    )

    trx_queue = queue.Queue(500)

    dispatcher = TransactionDispatcher(config.OUTPUT_MYSQL,
                                       trx_queue=trx_queue,
                                       prev_execution=prev_execution,
                                       max_connections=20)
    dispatcher.start()

    try:
        for trx in stream:
            if args.dry_run:
                print(trx)
            else:
                trx_queue.put(trx) # will block if full
    except KeyboardInterrupt:
        print('KeyboardInterrupt, Control-C or error in threads, exiting')
        main_logger.info('KeyboardInterrupt, Control-C or error in threads, exiting')
    except Exception as e:
        main_logger.info(e)
        raise
    finally:
        stream.close()
        dispatcher.quit()
        dispatcher.join()
        main_logger.info('APP END')

if __name__ == "__main__":
    main()
