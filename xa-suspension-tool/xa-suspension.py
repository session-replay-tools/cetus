#!/usr/bin/env python
# -*- coding:utf-8 -*-

from multiprocessing import Pool
import os
import re
import time
import json
import logging
import datetime
import MySQLdb

CONFIG = {"logs": "/data/cetus/xa_suspension_logs/xa-suspension.log",
          "backend": "/home/mysql-cetus/cetus_install/conf/cetus.conf",
          "user": "/home/mysql-cetus/cetus_install/conf/users.json",
          "temp_file": "/data/cetus/xa_suspension_logs/"
         }
BACKEND_CONF = []
USER_CONF = []

'''
悬挂事务查找模块
'''

def set_search_log():
    """日志记录"""

    global CONFIG
    logger = logging.getLogger()
    logger.setLevel(logging.NOTSET)
    log_path = CONFIG['logs']
    hdlr = logging.FileHandler(log_path)
    logger.addHandler(hdlr)
    log_format = "%(asctime)s - %(levelname)s - %(message)s"
    time_format = "%m/%d/%Y %H:%M:%S %p"
    formatter = logging.Formatter(log_format, time_format)
    hdlr.setFormatter(formatter)
    return logger

def get_config():
    """读取cetus后端配置信息"""

    global CONFIG
    global BACKEND_CONF
    global USER_CONF

    back_path = CONFIG['backend']
    with open(back_path, 'r') as backend_conf_file:
        conf_info = backend_conf_file.read()
        line = re.findall(r"proxy-backend-addresses.*", conf_info)[0]
        BACKEND_CONF = re.findall(r"(\d+\.\d+\.\d+\.\d+)(\:\d{4})?", line)
        for x in BACKEND_CONF:
            if not x[1]:
                x[1] = ':3306'

    user_path = CONFIG['user']
    with open(user_path, 'r') as user_conf_file:
        data = json.load(user_conf_file)
        USER_CONF = data['users']

def xa_recover(backend_conf):
    """读取mysql中xa recover的内容"""

    global USER_CONF

    host = backend_conf[0]
    port = backend_conf[1][1:]
    user = USER_CONF[0]['user']
    passwd = USER_CONF[0]['server_pwd']

    config = {
        'host': backend_conf[0],
        'port': int(backend_conf[1][1:]),
        'user': USER_CONF[0]['user'],
        'passwd': USER_CONF[0]['server_pwd']
    }

    try:
        db = MySQLdb.connect(**config)
        cursor = db.cursor()
        sql = "xa recover"
        cursor.execute(sql)
        results = cursor.fetchall()
    except Exception, e:
     #   logging.error("mysql: %s:%s xa recover 查询失败! %s,%s", host, port, Exception, e)
        return False
    else:
        xid_list = []
        if results:
            for i in results:
                xid_list.append(i[3])
        return xid_list


def get_suspension_xid(xid_list, next_xid_list):
    """读取xa recover中长时间处于悬挂的事务xid列表"""

    final_xid_list = []
    for i in xid_list:
        for j in next_xid_list:
            if i == j:
                final_xid_list.append(i)
    return final_xid_list

def get_all_suspesion_xid():
    """读取间隔时间的所有后端xa recover中的xid列表，确定长时间悬挂的事务"""

    global BACKEND_CONF

    final_xid = []
    all_backend_xid = map(xa_recover, BACKEND_CONF)
    
    time.sleep(10)

    all_backend_xid_next = map(xa_recover, BACKEND_CONF)

    for i in range(len(BACKEND_CONF)):
        final_xid_list = get_suspension_xid(all_backend_xid[i], all_backend_xid_next[i])
        final_xid.append(final_xid_list)
    return final_xid

def xid_final_is_null(final_xid):
    """判断final_xid是否为空"""

    element_number = 0
    for i in final_xid:
        if not i:
            continue
        else:
            element_number += 1
    return bool(element_number)

def get_all_xid(final_xid):
    """将所有后端xid去重，存入字符串"""
   
    all_xid = []
    for i in final_xid:
        all_xid.extend(i)
    all_xid = list(set(all_xid))
    return all_xid

def get_binlog_name(host, port):
    """读取mysql中show binlog的内容，获取binlog列表"""

    global USER_CONF

    config = {
        'host': host,
        'port': int(port),
        'user': USER_CONF[0]['user'],
        'passwd': USER_CONF[0]['server_pwd']
    }
    try:
        db = MySQLdb.connect(**config)
        cursor = db.cursor()
        sql = "show binary logs"
        cursor.execute(sql)
        results = cursor.fetchall()
    except Exception, e:
     #   logging.error("mysql: %s:%s show binary logs 查询失败! %s,%s", host, port, Exception, e)
        return False
    else:
        length = len(results)
        binlog_name = []
        for i in range(length):
            binlog_name.append(results[length - i - 1][0])
        return binlog_name

def get_binlog(host, port, binlog_name, xid_time):
    """读取binlog中指定时间的日志"""

    global USER_CONF
    global CONFIG

    user = USER_CONF[0]['user']
    passwd = USER_CONF[0]['server_pwd']
    current_time = time.strftime('%Y-%m-%d', time.localtime())
    current_hour = time.strftime("%H", time.localtime())

    mysql_link = 'mysqlbinlog -R --start-datetime="' + current_time + ' ' + xid_time +\
                 ':00:00" --stop-datetime="' + current_time + ' ' + str(int(xid_time)+1) +\
                 ':00:00" -h' + host +'  -u' + user + ' -p' + passwd + ' -P' + port + ' -vv '

    if xid_time == '23' and int(current_hour) == 0:
        yesterday = (datetime.date.today() - datetime.timedelta(days=1)).strftime("%Y-%m-%d")
        mysql_link = 'mysqlbinlog -R --start-datetime=\"' + yesterday + ' ' + xid_time +\
                     ':00:00" --stop-datetime="' + current_time + ' ' + '00:00:00" -h' +\
                     host +'  -u' + user + ' -p' + passwd + ' -P' + port + ' -vv '

    sql = binlog_name + ' 2>/dev/null|egrep "^XA(.*),1$" >> ' +\
          CONFIG['temp_file'] + host + port +'.txt 2>/dev/null' 

    os.system(mysql_link + sql)

def grep_status(host, port, xid):

    global CONFIG

    shell = 'cat ' + CONFIG['temp_file'] + host + port +\
            '.txt|grep ' + xid.encode('hex') + ' 2>/dev/null'
    grep_status_file = os.popen(shell)
    r = grep_status_file.read()
    grep_status_file.close()
    status_list = re.findall(r"XA\s(\w+)", r)
    return status_list

def get_status(args):

    global USER_CONF
    global CONFIG

    host = args[0][0]
    port = args[0][1][1:]
    all_xid = args[1]
    xid_time_dict = args[2]

    time_list = xid_time_dict.keys()

    user = USER_CONF[0]['user']
    passwd = USER_CONF[0]['server_pwd']    
    binlog_name = get_binlog_name(host, port)

    status = {}

    if not binlog_name:
        return status
        
    for xid_time in time_list:
        os.system('rm ' + CONFIG['temp_file'] + host + port +'.txt 2>/dev/null')
        for binlog in binlog_name: 
            get_binlog(host, port, binlog, xid_time)
            binlog_start = os.popen('mysqlbinlog -R -h' + host + ' -u' + user + ' -p' + 
                                    passwd + ' -P' + port + ' -vv ' + binlog + 
                                    ' 2>/dev/null|egrep "^#(\w{6})" 2>/dev/null|head -n 1')
            r = binlog_start.read()
            binlog_start.close()
            binlog_start_time = re.findall(r"^#(\d{6})", r)
            current_time = time.strftime('%y%m%d', time.localtime())
            if binlog_start_time[0] < current_time:
                break

        xid_is_time_same = xid_time_dict[xid_time]

        for xid in xid_is_time_same:
            status_list = grep_status(host, port, xid)
            print "-------status_list----------"
            print status_list

            if status_list:
                if 'COMMIT' in status_list:
                    logging.info("Mysql后端:%s:%s,悬挂事务xid:%s,最终状态:COMMIT", host, port, xid)
                    status[xid] = ('COMMIT')
                    continue
                elif 'ROLLBACK' in status_list:
                    logging.info("Mysql后端:%s:%s,悬挂事务xid:%s,最终状态:ROLLBACK", host, port, xid)
                    status[xid] = ('ROLLBACK')
                    continue
                elif 'PREPARE' in status_list:
                    logging.info("Mysql后端:%s:%s,悬挂事务xid:%s,最终状态:PREPARE", host, port, xid)
                    status[xid] = ('PREPARE')
                    continue
                elif 'END' in status_list:
                    logging.info("Mysql后端:%s:%s,悬挂事务xid:%s,最终状态:END", host, port, xid)
                    status[xid] = ('END')
                    continue
                elif 'START' in status_list:
                    logging.info("Mysql后端:%s:%s,悬挂事务xid:%s,最终状态:START", host, port, xid)
                    status[xid] = ('START')
                    continue
            else:
                logging.info("Mysql后端:%s:%s,没有该悬挂事务xid:%s", host, port, xid)
                status[xid] = ('NULL')
                continue
    print "-------status----------"
    print status
    return status


def suspension_status(final_xid):
    """获得所有后端的xid的最终状态"""

    global BACKEND_CONF

    all_xid = get_all_xid(final_xid)

    xid_time_dict = {}
    for i in range(len(all_xid)):
        xid_time = re.findall(r"_(\d{2})_", all_xid[i])
        if xid_time[0] in xid_time_dict.keys():
            xid_time_dict[xid_time[0]].append(all_xid[i])
        else:
            xid_time_dict.setdefault(xid_time[0],[all_xid[i]])

    args = []
    for m in BACKEND_CONF:
        mysql = []
        mysql.append(m)
        mysql.append(all_xid)
        mysql.append(xid_time_dict)
        args.append(mysql)

    print "-------all_xid----------"
    print all_xid
    pool = Pool(len(BACKEND_CONF))
    status = pool.map(get_status, args)
    print "============== status  all============="
    print status

    pool.close()
    final_status = []
    for xid in all_xid:
        xid_status = []
        for j in range(len(BACKEND_CONF)):
            if status[j].has_key(xid):
                xid_status.append(status[j][xid])

        if 'COMMIT' in xid_status:
            final_status.append('COMMIT')
            continue
        elif 'ROLLBACK' in xid_status:
            final_status.append('ROLLBACK')
            continue
        elif 'PREPARE' in xid_status:
            final_status.append('PREPARE')
            continue 
        elif 'END' in xid_status:
            final_status.append('END')
            continue
        elif 'START' in xid_status:
            final_status.append('START')
            continue
        else:
            final_status.append('NULL')
            continue
    return final_status, all_xid


'''
悬挂事务处理模块
'''

#在mysql中执行commit操作
def xa_commit(backend_conf, xid):

    global USER_CONF

    config = {
        'host': backend_conf[0],
        'port': int(backend_conf[1][1:]),
        'user': USER_CONF[0]['user'],
        'passwd': USER_CONF[0]['server_pwd']
    }
    try:
        db = MySQLdb.connect(**config)
        cursor = db.cursor()
        sql_set = "set autocommit = on"
        sql = "xa commit '%s'"%xid
        cursor.execute(sql_set)
        cursor.execute(sql)
        db.commit()
        logging.info("mysql: %s:%s xid:%s 提交成功!", 
                     config['host'], config['port'], xid)
    except Exception, e:
        logging.error("mysql: %s:%s xid:%s 提交失败! %s,%s", 
                      config['host'], config['port'], xid, Exception, e)
        return False

#在mysql中执行rollback操作
def xa_rollback(backend_conf, xid):

    global USER_CONF

    config = {
        'host': backend_conf[0],
        'port': int(backend_conf[1][1:]),
        'user': USER_CONF[0]['user'],
        'passwd': USER_CONF[0]['server_pwd']
    }

    try:
        db = MySQLdb.connect(**config)
        cursor = db.cursor()
        sql_set = "set autocommit = on"
        sql = "xa rollback '%s';"%xid
        cursor.execute(sql_set)
        cursor.execute(sql)
        db.commit()
        logging.info("mysql: %s:%s xid:%s 回滚成功!", 
                     config['host'], config['port'], xid)
    except Exception, e:
        logging.error("mysql: %s:%s xid:%s 回滚失败! %s,%s", 
                      config['host'], config['port'], xid, Exception, e)
        return False

#根据悬挂事务的最终状态，处理悬挂事务
def handle_suspension(final_xid, status, all_xid):

    global BACKEND_CONF

    for i in range(len(BACKEND_CONF)):
        for xid in final_xid[i]:
            for j in range(len(all_xid)):
                if xid == all_xid[j]:
                    if status[j] in 'PREPARE ROLLBACK END START':
                        xa_rollback(BACKEND_CONF[i], xid)
                    if status[j] == 'COMMIT':
                        xa_commit(BACKEND_CONF[i], xid)


def main():
    """功能主体"""

    get_config()
    logging = set_search_log()
    while True:
        try:
            final_xid = get_all_suspesion_xid()
        except Exception, e:
            logging.error("后端连接有问题！%s,%s", Exception, e)
            continue
        if xid_final_is_null(final_xid):
            logging.info("发现悬挂事务，开始处理！")
            (status, all_xid) = suspension_status(final_xid)
            logging.info("状态查找结束!")
            handle_suspension(final_xid, status, all_xid)
        else:
            continue

if __name__ == '__main__':
    main()
