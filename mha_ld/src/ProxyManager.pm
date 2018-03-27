#!/usr/bin/env perl
## author: cch
## version:2015.8.12
## version:2016.6.12 added by cch delete croak
## version:2016.8.3 modified by cch add parallel process
## version:2016.8.29 modified by cch ip:port
## desc:

package MHA::ProxyManager;

#use strict;
use warnings FATAL => 'all';
use English qw(-no_match_vars);
use Carp qw(croak);
use Getopt::Long qw(:config pass_through);
use Log::Dispatch;
use Log::Dispatch::File;
use Log::Dispatch::Screen;
use MHA::NodeUtil;
use MHA::Config;
use MHA::ServerManager;
use MHA::FileStatus;
use MHA::ManagerUtil;
use MHA::ManagerConst;
use MHA::HealthCheck;
use File::Basename;
use Parallel::ForkManager;
use Sys::Hostname;

my $g_global_config_file = $MHA::ManagerConst::DEFAULT_GLOBAL_CONF;
my $g_proxy_config_file;
my $g_config_file;
my $log_intera;
my $log;
my $error_code=0;
my @middle_ipport;
my $middle_ip;
my $middle_port;
my $middle_user;
my $middle_pass;
my @phones = @MHA::ManagerConst::MOBILE_PHONES;

sub send_alert($) {
  my $msg  = shift;
  my $eachline;

  unless(@phones) {
    @phones = @MHA::ManagerConst::MOBILE_PHONES;
  }
  foreach $eachline(@phones){
      #print $eachline . "\n";
      chomp($eachline);
      $eachline =~ s/\s//g;
      my $str="";
      eval { my $ret = system($str . " &");};
    }
}

sub gettime {
  my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) = localtime;
  $year += 1900; # $year是从1900开始计数的，所以$year需要加上1900；
  $mon += 1; # $mon是从0开始计数的，所以$mon需要加上1；
  my $datetime = sprintf ("%d-%02d-%02d %02d:%02d:%02d", $year,$mon,$mday,$hour,$min,$sec);
  return $datetime;
}

sub read_proxy_config($) {
  my $confile = shift;
  open(FILE,"<","$confile")||die "cannot open the file: $!\n";
  @linelist=<FILE>;
  foreach $eachline(@linelist){
#    print $eachline;
    chomp($eachline);
    if ( $eachline ne "" ) {
        my ( $name, $value ) = split( /=/, $eachline );
#       print $name;    
        $name =~ s/\s//g;
        $name =~ tr/[A-Z]/[a-z]/;
#       print $name . "\n";
        if ( substr($name,0,1) ne "#" ) {
            $value =~ s/\s//g;
#            print $value . "\n";    
            if ( $name eq "middle_ipport" ) {
               @middle_ipport = split(/,/, $value);
            }
            if ( $name eq "middle_user" ) {
               $middle_user = $value ;  
            }
            if ( $name eq "middle_pass" ) {
               $middle_pass = $value ;  
            }
        }  ## if
#        print $middle_user ."\n";
#        print $middle_pass ."\n";
    }  ## if
  } ## foreach

#  print $middle_user ."\n";
#  print $middle_pass ."\n";

#  for ( my $i = 0 ; $i < @middle_ipport ; $i++ ) {
#     print "$middle_ipport[$i]" ."\n";
#  }
  close FILE;
}


#sub init_config($) {
#  my $logfile = shift;
#  $log = MHA::ManagerUtil::init_log($logfile);
#}

sub init_config($$) {
  my $logfile = shift;
  my $confile = shift;

  $g_config_file = $confile;
  $log = MHA::ManagerUtil::init_log($logfile);

  my ( $sc_ref, $binlog_ref ) = new MHA::Config(
    logger     => $log,
    globalfile => $g_global_config_file,
    file       => $g_config_file
  )->read_config();
  my @servers_config        = @$sc_ref;
  my @binlog_servers_config = @$binlog_ref;

   unless ($g_proxy_config_file) {
    if ( $servers_config[0]->{proxy_conf} ) {
      $g_proxy_config_file = $servers_config[0]->{proxy_conf};
    }
    else {
      $g_proxy_config_file = "/masterha/app1/app1.cnf";
    }
  }
}

sub init_config_intera($) {
  my $confile = shift;

  $log_intera = MHA::ManagerUtil::init_log();
  $g_config_file = $confile;

  my ( $sc_ref, $binlog_ref ) = new MHA::Config(
    logger     => $log_intera,
    globalfile => $g_global_config_file,
    file       => $g_config_file
  )->read_config();
  my @servers_config        = @$sc_ref;
  my @binlog_servers_config = @$binlog_ref;

   unless ($g_proxy_config_file) {
    if ( $servers_config[0]->{proxy_conf} ) {
      $g_proxy_config_file = $servers_config[0]->{proxy_conf};
    }
    else {
      $g_proxy_config_file = "/masterha/app1/app1.cnf";
    }
  }
}

sub setdb ($$$$$) { 
  my $status      = shift;
  my $mtype       = shift;
  my $address     = shift;
  my $lf          = shift;
  my $confile     = shift;
  my $ret;
  my $exit_flag = 0;

  init_config("$lf","$confile");
  $log->info("[ProxyManager::setdb] proxy_conf: $g_proxy_config_file ");
  read_proxy_config("$g_proxy_config_file");

##  croak "test --------- ProxyManager.pm ------------ test ";
#  for ( my $i = 0 ; $i < @middle_ip ; $i++ ) {
##     print "$middle_ip[$i]" ."\n";
#     my $command_update = "/usr/bin/mysql -h$middle_ip[$i] -u$middle_user -P$middle_port -p$middle_pass "
#        ."-e \" update backends set state=\'$status\' , type=\'$mtype\' where address=\'$address\';\"";
#     $log->info("exec command: $command_update");
#
##     croak "test --------- ProxyManager.pm ------------ test ";
#     eval{ $ret = system($command_update . " &"); };
#  
#     if ( $@ || $ret != 0) {
#        $log->error($@);
#        $log->error($ret);
##        croak;
#        $exit_flag=1;
#     } ##if
#  } ## for

#### delete  
##  if ($exit_flag == 1) {
##    croak ;
##  }

  my $pm = new Parallel::ForkManager( $#middle_ipport + 1 );
  my $pm_failed   = 0;
  $pm->run_on_start(
    sub {
      my ( $pid, $ident ) = @_;
      #print "** $ident started, pid: $pid\n";
      $log->info("** $ident started, pid: $pid");
    }
  );
  $pm->run_on_finish(
    sub {
      my ( $pid, $exit_code, $ident ) = @_;
      $pm_failed = 1 if ($exit_code);
      #print "** $ident is over ".
      #  "with PID $pid and exit code: $exit_code\n";
      $log->info("** $ident is over with PID $pid and exit code: $exit_code");
    }
  );
  $pm->run_on_wait(
    sub {
      #print "** waiting for other child's process...\n";
      $log->info("** waiting for other child's process...");
    },3
  );

  foreach my $proxychild (@middle_ipport) {
    $pm->start($proxychild) and next;
    eval {
      $SIG{INT} = $SIG{HUP} = $SIG{QUIT} = $SIG{TERM} = "DEFAULT";
      #print $proxychild;
      my @temp = split ":",$proxychild;
      $middle_ip = substr($proxychild,0,length($temp[0]));
      $middle_port = substr($proxychild,length($temp[0])+1);
      my $command_update = "/usr/bin/mysql -h$middle_ip -u$middle_user -P$middle_port -p$middle_pass  "
           ."-e \" update backends set state=\'$status\' , type=\'$mtype\' where address=\'$address\';\"";
      $log->info("exec command: $command_update");
      my $rc = system($command_update);
      my $gt=gettime;
      if ($rc) {
        send_alert("$gt MHA node($address). Proxy's info changed $status and $mtype. Fail!");
        eval {my $rc_again = system($command_update . " &")};
      }
      else {
        send_alert("$gt MHA node($address). Proxy's info changed $status and $mtype. Succ!");    
      }
      $pm->finish($rc);
    }; ##eval

    if ($@) {
      $log->error($@);
      undef $@;
      $pm->finish(1);
    }
    $pm->finish(0);
  } ## foreach

  #$pm->wait_all_children;

  $log->info("......setdb.....");
  return 0;
}

sub setdb_intera ($$$$) {
  my $status      = shift;
  my $mtype       = shift;
  my $address     = shift;
  my $confile     = shift;
  my $ret;
  my $exit_flag = 0;
  
  init_config_intera("$confile");
  $log_intera->info("[ProxyManager::setdb_intera] proxy_conf: $g_proxy_config_file ");

  read_proxy_config("$g_proxy_config_file");
##  croak "test --------- ProxyManager.pm ------------ test ";

#  for ( my $i = 0 ; $i < @middle_ip ; $i++ ) {
#     print "$middle_ip[$i]" ."\n";
#     my $command_update = "/usr/bin/mysql -h$middle_ip[i] -u$middle_user -P$middle_port -p$middle_pass "
#         ."-e \" update backends set state=\'$status\' , type=\'$mtype\' where address=\'$address\';\"";
#     $log_intera->info("exec command: $command_update");

##     croak "test --------- ProxyManager.pm ------------ test ";
#     eval{ $ret = system($command_update . " &"); };
#
#     if ( $@ || $ret != 0) {
#        $log_intera->error($@);
#        $log_intera->error($ret);
#        $exit_flag = 1;
#        croak;
#     }
#  } ## for

##### delete
##  if ($exit_flag == 1) {
##     croak;
##   }

  my $pm = new Parallel::ForkManager( $#middle_ipport + 1 );
  my $pm_failed   = 0;
  $pm->run_on_start(
    sub {
      my ( $pid, $ident ) = @_;
      #print "** $ident started, pid: $pid\n";
      $log_intera->info("** $ident started, pid: $pid");
    }
  );
  $pm->run_on_finish(
    sub {
      my ( $pid, $exit_code, $ident ) = @_;
      $pm_failed = 1 if ($exit_code);
      #print "** $ident is over ".
      #  "with PID $pid and exit code: $exit_code\n";
      $log_intera->info("** $ident is over with PID $pid and exit code: $exit_code");
    }
  );
  $pm->run_on_wait(
    sub {
      #print "** waiting for other child's process...\n";
      $log_intera->info("** waiting for other child's process...");
    },3
  );

  foreach my $proxychild (@middle_ipport) {
    $pm->start($proxychild) and next;
    eval {
      $SIG{INT} = $SIG{HUP} = $SIG{QUIT} = $SIG{TERM} = "DEFAULT";
      #print $proxychild;
      my @temp = split ":",$proxychild;
      $middle_ip = substr($proxychild,0,length($temp[0]));
      $middle_port = substr($proxychild,length($temp[0])+1);
      my $command_update = "/usr/bin/mysql -h$middle_ip -u$middle_user -P$middle_port -p$middle_pass  "
           ."-e \" update backends set state=\'$status\' , type=\'$mtype\' where address=\'$address\';\"";
      $log_intera->info("exec command: $command_update");
      my $rc = system($command_update);
      my $gt=gettime;
      if ($rc) {
        send_alert("$gt MHA node($address). Proxy's info changed $status and $mtype. Fail!");
        eval {my $rc_again = system($command_update . " &")};
      }
      else {
        send_alert("$gt MHA node($address). Proxy's info changed $status and $mtype. Succ!");    
      }
      $pm->finish($rc);
    }; ##eval

    if ($@) {
      $log_intera->error($@);
      undef $@;
      $pm->finish(1);
    }
    $pm->finish(0);
  } ## foreach

  #$pm->wait_all_children;

  $log_intera->info("......setdb.....");
  return 0;
}

sub setproxy ($$$$$$) {
  my $type         = shift;
  my $status       = shift; 
  my $addr         = shift;  
  my $dbtype       = shift;
  my $lf           = shift;
  my $confile      = shift;

  init_config("$lf","$confile");
  $log->info("[ProxyManager::setproxy] proxy_conf: $g_proxy_config_file "); 
#  croak "test --------- ProxyManager.pm ------------ test ";
 
  if ( $type eq "failover" ) {  
    $log->info("type   : $type");
    $log->info("status : $status");
    $log->info("addr   : $addr");
    $log->info("dbtype : $dbtype");
    $error_code = setdb("$status","$dbtype","$addr","$lf","$confile");
  }elsif ( $type eq "setmaster" ) {
    $log->info("type   : $type");
    $log->info("status : $status");
    $addr =~ /\((.*)\)/;
    $log->info("addr   : $1");
    $log->info("dbtype : $dbtype");
    $error_code = setdb("$status","$dbtype","$1","$lf","$confile");
  }elsif ( $type eq "setslave" ) {
    $log->info("type   : $type");
    $log->info("status : $status");
    $addr =~ /\((.*)\)/;
    $log->info("addr   : $1");
    $log->info("dbtype : $dbtype");
    $error_code = setdb("$status","$dbtype","$1","$lf","$confile");
  }else {
    $log->info("This type is not defined.");
  }

  if ($@) {
    $error_code = 1;
   }
   return $error_code;
}

sub setproxy_intera ($$$$$) {
  my $type         = shift;
  my $status       = shift;
  my $addr         = shift;
  my $dbtype       = shift; 
  my $confile      = shift;


  init_config_intera ("$confile");
  $log_intera->info("[ProxyManager::setproxy_intera] proxy_conf: $g_proxy_config_file ");
#  croak "test --------- ProxyManager.pm ------------ test ";

  if ( $type eq "failover" ) {
    $log_intera->info("type   : $type");
    $log_intera->info("status : $status");
    $log_intera->info("addr   : $addr");
    $log_intera->info("dbtype : $dbtype");
    $error_code = setdb_intera("$status","$dbtype","$addr","$confile");
  }elsif ( $type eq "setmaster" ) {
    $log_intera->info("type   : $type");
    $log_intera->info("status : $status");
    $addr =~ /\((.*)\)/;
    $log_intera->info("addr   : $1");
    $log_intera->info("dbtype : $dbtype");
    $error_code = setdb_intera("$status","$dbtype","$1","$confile");
  }elsif ( $type eq "setslave" ) {
    $log_intera->info("type   : $type");
    $log_intera->info("status : $status");
    $addr =~ /\((.*)\)/;
    $log_intera->info("addr   : $1");
    $log_intera->info("dbtype : $dbtype");
    $error_code = setdb_intera("$status","$dbtype","$1","$confile");
  }else {
    $log_intera->info("This type is not defined.");
  }

  if ($@) {
    $error_code = 1;
   }
   return $error_code;
}
1;


