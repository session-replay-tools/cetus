#!/bin/bash
## author : cch
## desc: sendmail

############ variable part ########################
conf=/masterha/app1/sample.cnf

if [ $# -ne 1 ];then
    mailsubject="mha--failover--`date +%Y%m%d%H%M`"
else
    mailsubject=$1
fi


############# main #########################
find_flag=`cat $conf|grep -v '^#'|grep "manager_workdir"|awk -F= '{print $2}'|wc -l`
if [  ${find_flag} -eq 1 ];then
    manager_workdir=`cat $conf|grep -v '^#'|grep "manager_workdir"|awk -F= '{print $2}'|sed 's/ //g'`
fi

if [ -e ${manager_workdir}/sendmail.txt ];then
    echo "${manager_workdir}/sendmail.txt" >> ./sendMail.log
#   send mail 
fi

echo `date` >> ./sendMail.log
