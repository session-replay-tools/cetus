#!/usr/bin/env perl

#  Copyright (C) 2011 DeNA Co.,Ltd.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#   along with this program; if not, write to the Free Software
#  Foundation, Inc.,
#  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

package MHA::SlaveUtil;

use strict;
use warnings FATAL => 'all';

use English qw(-no_match_vars);
use File::Basename;
use MHA::NodeUtil;
use DBI;

use constant Get_Version_SQL => "SELECT VERSION() AS Value";
use constant Get_Datadir_SQL => "SELECT \@\@global.datadir AS Value";
use constant Get_Relay_Log_Info_Type_SQL =>
  "SELECT \@\@global.relay_log_info_repository AS Value";
use constant Get_Relay_Log_File_SQL =>
  "SELECT Relay_log_name FROM mysql.slave_relay_log_info";
use constant Get_Relay_Log_Info_SQL =>
  "SELECT \@\@global.relay_log_info_file AS Value";
use constant Is_Relay_Purge_SQL => "SELECT \@\@global.relay_log_purge As Value";
use constant Show_Log_Error_File_SQL => "SHOW VARIABLES LIKE 'log_error'";
use constant Show_Slave_Status_SQL   => "SHOW SLAVE STATUS";
use constant Disable_Relay_Purge_SQL => "SET GLOBAL relay_log_purge=0";
use constant Enable_Relay_Purge_SQL  => "SET GLOBAL relay_log_purge=1";
use constant Flush_Relay_Logs_SQL =>
  "FLUSH NO_WRITE_TO_BINLOG /*!50501 RELAY */ LOGS";
use constant Get_Failover_Lock_SQL =>
  "SELECT GET_LOCK('MHA_Master_High_Availability_Failover', ?) AS Value";
use constant Release_Failover_Lock_SQL =>
  "SELECT RELEASE_LOCK('MHA_Master_High_Availability_Failover') As Value";
use constant Get_Monitor_Lock_SQL =>
  "SELECT GET_LOCK('MHA_Master_High_Availability_Monitor', ?) AS Value";
use constant Release_Monitor_Lock_SQL =>
  "SELECT RELEASE_LOCK('MHA_Master_High_Availability_Monitor') As Value";

sub get_variable($$) {
  my $dbh   = shift;
  my $query = shift;
  my $sth   = $dbh->prepare($query);
  $sth->execute();
  my $href = $sth->fetchrow_hashref;
  return $href->{Value};
}

sub get_version($) {
  my $dbh = shift;
  return get_variable( $dbh, Get_Version_SQL );
}

sub get_log_error_file($) {
  my $dbh = shift;
  return get_variable( $dbh, Show_Log_Error_File_SQL );
}

sub get_relay_log_info_type {
  my $dbh           = shift;
  my $mysql_version = shift;
  my $type;
  $mysql_version = get_version($dbh) unless ($mysql_version);
  if ( MHA::NodeUtil::mysql_version_ge( $mysql_version, "5.6.2" ) ) {
    $type = get_variable( $dbh, Get_Relay_Log_Info_Type_SQL );
  }
  unless ( defined($type) ) {
    $type = "FILE";
  }
  return $type;
}

sub get_relay_dir_file_from_table($) {
  my $dbh  = shift;
  my $sth  = $dbh->prepare(Get_Relay_Log_File_SQL);
  my $ret  = $sth->execute();
  my $href = $sth->fetchrow_hashref;
  if ( !defined($href) || !defined( $href->{Relay_log_name} ) ) {
    return;
  }
  my $current_relay_log_file = $href->{Relay_log_name};
  my $datadir = get_variable( $dbh, Get_Datadir_SQL );
  my $relay_dir;
  unless ( $current_relay_log_file =~ m/^\// ) {
    $current_relay_log_file =~ s/^\.\///;
    $current_relay_log_file = $datadir . "/" . $current_relay_log_file;
    $relay_dir              = $datadir;
    $relay_dir =~ s/\/$//;
  }
  else {
    $relay_dir = dirname($current_relay_log_file);
  }
  my $relay_log_basename = basename($current_relay_log_file);
  return ( $relay_dir, $relay_log_basename );
}

sub get_relay_log_info_path {
  my $dbh           = shift;
  my $mysql_version = shift;
  my $relay_log_info_path;
  my $filename;
  my $datadir = get_variable( $dbh, Get_Datadir_SQL );
  $mysql_version = get_version($dbh) unless ($mysql_version);
  if ( MHA::NodeUtil::mysql_version_ge( $mysql_version, "5.1.0" ) ) {
    $filename = get_variable( $dbh, Get_Relay_Log_Info_SQL );
  }

#relay_log_info_file was introduced in 5.1. In 5.0, it's fixed to "relay_log.info"
  if ( !defined($filename) ) {
    $filename = "relay-log.info";
  }
  unless ( $filename =~ m/^\// ) {
    $filename =~ s/^\.\///;
    $datadir =~ s/\/$//;
    $relay_log_info_path = $datadir . "/" . $filename;
  }
  else {
    $relay_log_info_path = $filename;
  }
  return $relay_log_info_path;
}

sub is_relay_log_purge($) {
  my $dbh = shift;
  return get_variable( $dbh, Is_Relay_Purge_SQL );
}

sub disable_relay_log_purge($) {
  my $dbh = shift;
  $dbh->do(Disable_Relay_Purge_SQL);
}

sub purge_relay_logs($) {
  my $dbh = shift;
  $dbh->do(Enable_Relay_Purge_SQL);
  $dbh->do(Flush_Relay_Logs_SQL);

  # To (almost) make sure that relay log is switched(purged) before setting
  # relay_log_purge=0;
  sleep 3;
  $dbh->do(Disable_Relay_Purge_SQL);
  return 0;
}

sub is_slave($) {
  my $dbh   = shift;
  my $query = Show_Slave_Status_SQL;
  my $sth   = $dbh->prepare($query);
  my $ret   = $sth->execute();
  if ( !defined($ret) || $ret != 1 ) {
    return 0;
  }
  return 1;
}

sub get_advisory_lock_internal($$$) {
  my $dbh     = shift;
  my $timeout = shift;
  my $query   = shift;
  my $sth     = $dbh->prepare($query);
  my $ret     = $sth->execute($timeout);
  my $href    = $sth->fetchrow_hashref;
  if ( !defined($href) || !defined( $href->{Value} ) ) {
    return 2;
  }
  elsif ( $href->{Value} != 1 ) {
    return 1;
  }
  return 0;
}

sub release_advisory_lock_internal($$) {
  my $dbh   = shift;
  my $query = shift;
  my $sth   = $dbh->prepare($query);
  my $ret   = $sth->execute();
  my $href  = $sth->fetchrow_hashref;
  if ( !defined($href) || !defined( $href->{Value} ) || $href->{Value} != 1 ) {
    return 1;
  }
  return 0;
}

sub get_failover_advisory_lock {
  my $dbh     = shift;
  my $timeout = shift;
  return get_advisory_lock_internal( $dbh, $timeout, Get_Failover_Lock_SQL );
}

sub release_failover_advisory_lock($) {
  my $dbh = shift;
  return release_advisory_lock_internal( $dbh, Release_Failover_Lock_SQL );
}

sub get_monitor_advisory_lock {
  my $dbh     = shift;
  my $timeout = shift;
  return get_advisory_lock_internal( $dbh, $timeout, Get_Monitor_Lock_SQL );
}

sub release_monitor_advisory_lock($) {
  my $dbh = shift;
  return release_advisory_lock_internal( $dbh, Release_Monitor_Lock_SQL );
}

1;

