#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import logging
from transaction import Transaction


class PreviousExecution:
    def __init__(self, from_trx: str, trxs: list):
        gid, sid = from_trx.gtid.split(':')
        self.gtid_executed = gid+':1-'+sid #TODO: this is wrong, find better solution
        self.start_log_file = from_trx.binlog_file
        self.start_log_pos = from_trx.last_log_pos
        self.overlapped_gtids = {t.gtid:False for t in trxs}
        self._all_tested = False
        self._transactions = trxs

    def executed(self, gtid: str) -> bool:
        if self._all_tested:
            return False
        if self.overlapped_gtids.get(gtid) is not None: # True or False, but not None
            self.overlapped_gtids[gtid] = True # mark used
            self._all_tested = all(self.overlapped_gtids.values())
            trx = self._get_trx(gtid)
            # if XA_PREPARE doesn't have paired XA_COMMIT, we execute it again
            if trx.type == Transaction.TRX_XA_PREPARE:
                has_pair = self._has_paired_trx(trx)
                main_logger = logging.getLogger('main')
                main_logger.warning("gtid:{} does't have paired XA_COMMIT, will be executed again".format(gtid))
                return has_pair
            else:
                return True
        else:
            return False

    def _get_trx(self, gtid: str) -> Transaction:
        trx = next((t for t in self._transactions if t.gtid==gtid), None)
        assert trx is not None
        return trx

    def _has_paired_trx(self, preparetrx: Transaction) -> bool:
        return any(preparetrx.XID==trx.XID for trx in self._transactions\
                       if trx.type==Transaction.TRX_XA_COMMIT)

def reverse_readline(filename, buf_size=8192):
    """a generator that returns the lines of a file in reverse order"""
    with open(filename) as fh:
        segment = None
        offset = 0
        fh.seek(0, os.SEEK_END)
        file_size = remaining_size = fh.tell()
        while remaining_size > 0:
            offset = min(file_size, offset + buf_size)
            fh.seek(file_size - offset)
            buffer = fh.read(min(remaining_size, buf_size))
            remaining_size -= buf_size
            lines = buffer.split('\n')
            # the first line of the buffer is probably not a complete line so
            # we'll save it and append it to the last line of the next buffer
            # we read
            if segment is not None:
                # if the previous chunk starts right from the beginning of line
                # do not concact the segment to the last line of new chunk
                # instead, yield the segment first 
                if buffer[-1] is not '\n':
                    lines[-1] += segment
                else:
                    yield segment
            segment = lines[0]
            for index in range(len(lines) - 1, 0, -1):
                if len(lines[index]):
                    yield lines[index]
        # Don't yield None if the file was empty
        if segment is not None:
            yield segment

def read(filename) -> PreviousExecution:
    transactions = []
    for i, line in enumerate(reverse_readline(filename)):
        if i >= 50:
            break
        s = line.split()
        transactions.append(Transaction(s[2], s[3], s[4], s[5], int(s[6])))

    if len(transactions) < 1:
        return None

    transactions = sorted(transactions, key=lambda trx: trx.gtid)
    return PreviousExecution(transactions[0], transactions[1:])
        
