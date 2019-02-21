#! /usr/bin/env python3
# -*- coding: utf-8 -*-

from transaction import Transaction
import queue
import threading
import _thread
import pymysql
import os
import time
import logging
import signal
import traceback
from recovery import PreviousExecution

g_jobs = queue.Queue()
g_jobs_done = queue.Queue()
g_shutdown = False

def force_exit():
    os._exit(1)
    #os.kill(os.getpid(), signal.SIGKILL)

class MysqlConnection:
    '''wrapper class for pymysql.Connection
    it has a status and it remembers prepared XID
    '''

    XA_PREPARED = 'XA_PREPARED'
    IDLE = 'IDLE'
    BUSY = 'BUSY'

    def __init__(self, conn: pymysql.Connection):
        self.connection = conn
        self.status = self.IDLE
        self._xid = None
        self.xa_prepare_ignored = False

    @property
    def xid(self):
        assert self.status == self.XA_PREPARED
        return self._xid

    @xid.setter
    def xid(self, xid):
        self._xid = xid

    def __repr__(self):
        return 'mysql: '+str(self.connection.thread_id())
    

class Job:
    def __init__(self, trx: Transaction, conn: MysqlConnection):
        '''run in Dispatcher thread
        status change always happen in Dispatcher thread
        '''
        self._trx = trx
        self._conn = conn
        self._conn.status = MysqlConnection.BUSY

    def execute(self):
        '''run in worker threads
        '''
        global g_shutdown
        print("Job::execute", self)
        print(self._trx.brief())
        main_logger = logging.getLogger('main')
        trx_logger = logging.getLogger('trx')
        with self._conn.connection.cursor() as cursor:
            for sql in self._trx.sql_list:
                try:
                    main_logger.debug(str(self) + ' executing: ' + sql)
                    cursor.execute(sql)
                    main_logger.debug(str(self) + ' OK')
                except pymysql.err.Error as e:
                    thrd = threading.current_thread().name
                    main_logger.error(thrd + ' Failed SQL: ' + sql)
                    main_logger.error(thrd + str(e))
                    print(thrd + ' Failed SQL: ' + sql)
                    print(thrd + str(e))
                    if not g_shutdown:
                        g_shutdown = True
                        main_logger.warning(thrd + 'Sending KeyboardInterrupt to main thread')
                        print(thrd + 'Sending KeyboardInterrupt to main thread')
                        _thread.interrupt_main()
                    return
            trx_logger.info(self._trx)

    def after_execute(self):
        '''run in Dispatcher thread
        '''
        #print("after_execute", self)
        if self._trx.type == Transaction.TRX_XA_PREPARE:
            self._conn.xid = self._trx.XID
            self._conn.status = MysqlConnection.XA_PREPARED
            self._conn.xa_prepare_ignored = (len(self._trx.sql_list) == 0)
            main_logger = logging.getLogger('main')
            main_logger.debug('Hold conn: {} for XA PREPARE: {}'.format(
                self._conn, self._trx.XID))
        else:
            self._conn.status = MysqlConnection.IDLE
            self._conn.xa_prepare_ignored = False

    def __repr__(self):
        return '{} {} {}'.format(threading.current_thread().name,
                                 self._trx.gtid, self._conn)
        

def _worker():
    while True:
        job = g_jobs.get(block=True)
        if job is not None:
            job.execute()
            g_jobs_done.put(job)
        else:
            # thread exit
            main_logger = logging.getLogger('main')
            main_logger.info(threading.current_thread().name + ' exit')
            g_jobs.put(None)
            break


class TransactionDispatcher(threading.Thread):
    def __init__(self, mysql_config: dict,
                 trx_queue: queue.Queue=None,
                 prev_execution: PreviousExecution=None,
                 max_connections: int=20):
        super().__init__(name='Dispatcher-Thread')
        self._mysql_config = mysql_config
        self._mysql_config['autocommit'] = True
        self._connections = []
        self._max_connections = max_connections
        self._trx_queue = trx_queue
        self._threads = set()
        self._create_threads()
        self._running_transactions = set()
        self._prev_execution = prev_execution

    def quit(self):
        global g_shutdown
        g_shutdown = True
        if self._trx_queue.empty():
            self._trx_queue.put(None)

    def _create_threads(self):
        for i in range(self._max_connections):
            new_th = threading.Thread(target=_worker) # TODO: weak ref
            new_th.start()
            self._threads.add(new_th)
            main_logger = logging.getLogger('main')
            main_logger.info('New thread: ' + new_th.name)

    def _can_do_parallel(self, trx: Transaction) -> bool:
        if len(self._running_transactions) == 0:
            return True

        # interleaved with any running transaction
        return any(trx.interleaved(rt) for rt in self._running_transactions)

    def _get_appropriate_connection(self, trx: Transaction) -> MysqlConnection:
        main_logger = logging.getLogger('main')
        if len(self._connections) < self._max_connections:
            try:
                conn = pymysql.connect(**self._mysql_config)
            except pymysql.err.OperationalError as e:
                main_logger.error(e)
                traceback.print_exc()
                force_exit()

            mycon = MysqlConnection(conn)
            self._connections.append(mycon)
            main_logger.info('New conn: ' + str(mycon))

        if trx.type == Transaction.TRX_XA_COMMIT:
            conn =  next((c for c in self._connections \
                        if c.status==MysqlConnection.XA_PREPARED \
                        and c.xid==trx.XID), None)
            if conn is None:
                main_logger.warning('XA commit {} cannot pairing, wait...'.format(trx.XID))
            else:
                main_logger.debug('Got pair conn: {} for XA COMMIT: {}'.format(conn, trx.XID))

            # if the corresponding XA_PREPARE transaction is ignored,
            # also ignore the COMMIT. TODO: should not modify trx here
            if conn and conn.xa_prepare_ignored:
                main_logger.warning('XA prepared ignored, also ignoring XA commit')
                trx.sql_list = []
            return conn
        else:
            conn = next((c for c in self._connections \
                        if c.status==MysqlConnection.IDLE), None)
            if conn is None:
                main_logger.warning('No idle conn for {} {}'.format(trx.gtid, trx.type))
            return conn

    def _complete_jobs(self) -> int:
        count = 0
        while True:
            try:
                '''
                If we got one vaccant thread (count>0), we get a chance to arrange
                new jobs. And we need to clear the queue in non-blocking mode.
                '''
                job = g_jobs_done.get(block=(count==0), timeout=0.5)
                job.after_execute()
                self._running_transactions.remove(job._trx)
                count += 1
            except queue.Empty:
                break
        return count

    def _drain_jobs(self, filename: str) -> int:
        '''The `interval rule` only works inside one binlog file
        When new file comes, all jobs in last file must complete
        '''
        main_logger = logging.getLogger('main')
        count = 0
        while len(self._running_transactions) > 0:
            main_logger.info('Draining jobs in file: %s', filename)
            try:
                job = g_jobs_done.get(timeout=5)
                job.after_execute()
                self._running_transactions.remove(job._trx)
                count += 1
            except queue.Empty:
                break
        return count

    def _join_all_threads(self):
        g_jobs.put(None)
        while any(t.is_alive() for t in self._threads):
            time.sleep(0.5)

    def run(self):
        main_logger = logging.getLogger('main')
        global g_shutdown
        current_file = ''
        while True:
            if g_shutdown:
                self._join_all_threads()
                main_logger.info(threading.current_thread().name + ' exit')
                break

            trx = self._trx_queue.get()

            if trx is None or \
               (self._prev_execution and self._prev_execution.executed(trx.gtid)):
                continue

            if trx.binlog_file != current_file:
                self._drain_jobs(current_file)
                current_file = trx.binlog_file

            if self._can_do_parallel(trx): #ensure parallel probing before get connection
                conn = self._get_appropriate_connection(trx) # can return None
            else:
                conn = None

            while conn is None or not self._can_do_parallel(trx):
                self._complete_jobs() # will block if no available thread
                if conn is None:
                    conn = self._get_appropriate_connection(trx)
                global g_shutdown
                if g_shutdown is True:
                    break
            else:
                self._running_transactions.add(trx)
                g_jobs.put(Job(trx, conn))


