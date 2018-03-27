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

package MHA::NodeUtil;

use strict;
use warnings FATAL => 'all';

use Carp qw(croak);
use MHA::NodeConst;
use File::Path;
use Errno();

sub create_dir_if($) {
  my $dir = shift;
  if ( !-d $dir ) {
    eval {
      print "Creating directory $dir.. ";
      mkpath($dir);
      print "done.\n";
    };
    if ($@) {
      my $e = $@;
      undef $@;
      if ( -d $dir ) {
        print "ok. already exists.\n";
      }
      else {
        croak "failed to create dir:$dir:$e";
      }
    }
  }
}

# Compare file checksum between local and remote host
sub compare_checksum {
  my $local_file  = shift;
  my $remote_path = shift;
  my $ssh_user    = shift;
  my $ssh_host    = shift;
  my $ssh_port    = shift;
  $ssh_port = 22 unless ($ssh_port);

  my $local_md5 = `md5sum $local_file`;
  return 1 if ($?);
  chomp($local_md5);
  $local_md5 = substr( $local_md5, 0, 32 );
  my $ssh_user_host = $ssh_user . '@' . $ssh_host;
  my $remote_md5 =
`ssh $MHA::NodeConst::SSH_OPT_ALIVE -p $ssh_port $ssh_user_host \"md5sum $remote_path\"`;
  return 1 if ($?);
  chomp($remote_md5);
  $remote_md5 = substr( $remote_md5, 0, 32 );
  return 2 if ( $local_md5 ne $remote_md5 );
  return 0;
}

sub file_copy {
  my $to_remote   = shift;
  my $local_file  = shift;
  my $remote_file = shift;
  my $ssh_user    = shift;
  my $ssh_host    = shift;
  my $log_output  = shift;
  my $ssh_port    = shift;
  $ssh_port = 22 unless ($ssh_port);

  my $ssh_user_host = $ssh_user . '@' . $ssh_host;
  my ( $from, $to );
  if ($to_remote) {
    $from = $local_file;
    $to   = "$ssh_user_host:$remote_file";
  }
  else {
    $to   = $local_file;
    $from = "$ssh_user_host:$remote_file";
  }

  my $max_retries = 3;
  my $retry_count = 0;
  my $copy_fail   = 1;
  my $copy_command =
    "scp $MHA::NodeConst::SSH_OPT_ALIVE -P $ssh_port $from $to";
  if ($log_output) {
    $copy_command .= " >> $log_output 2>&1";
  }

  while ( $retry_count < $max_retries ) {
    if (
      system($copy_command)
      || compare_checksum(
        $local_file, $remote_file, $ssh_user, $ssh_host, $ssh_port
      )
      )
    {
      my $msg = "Failed copy or checksum. Retrying..";
      if ($log_output) {
        system("echo $msg >> $log_output 2>&1");
      }
      else {
        print "$msg\n";
      }
      $retry_count++;
    }
    else {
      $copy_fail = 0;
      last;
    }
  }
  return $copy_fail;
}

sub system_rc($) {
  my $rc   = shift;
  my $high = $rc >> 8;
  my $low  = $rc & 255;
  return ( $high, $low );
}

sub create_file_if {
  my $file = shift;
  if ( $file && ( !-f $file ) ) {
    open( my $out, ">", $file ) or croak "$!:$file";
    close($out);
  }
}

sub drop_file_if($) {
  my $file = shift;
  if ( $file && -f $file ) {
    unlink $file or croak "$!:$file";
  }
}

sub get_ip {
  my $host = shift;
  my ( $bin_addr_host, $addr_host );
  if ( defined($host) ) {
    $bin_addr_host = gethostbyname($host);
    unless ($bin_addr_host) {
      croak "Failed to get IP address on host $host!\n";
    }
    $addr_host = sprintf( "%vd", $bin_addr_host );
    return $addr_host;
  }
  return;
}

sub current_time() {
  my ( $sec, $min, $hour, $mday, $mon, $year ) = localtime();
  $mon  += 1;
  $year += 1900;
  my $val = sprintf( "%d-%02d-%02d %02d:%02d:%02d",
    $year, $mon, $mday, $hour, $min, $sec );
  return $val;
}

sub check_manager_version {
  my $manager_version = shift;
  if ( $manager_version < $MHA::NodeConst::MGR_MIN_VERSION ) {
    croak
"MHA Manager version is $manager_version, but must be $MHA::NodeConst::MGR_MIN_VERSION or higher.\n";
  }
}

sub parse_mysql_version($) {
  my $str = shift;
  my $result = sprintf( '%03d%03d%03d', $str =~ m/(\d+)/g );
  return $result;
}

sub parse_mysql_major_version($) {
  my $str = shift;
  my $result = sprintf( '%03d%03d', $str =~ m/(\d+)/g );
  return $result;
}

sub mysql_version_ge {
  my ( $my_version, $target_version ) = @_;
  my $result =
    parse_mysql_version($my_version) ge parse_mysql_version($target_version)
    ? 1
    : 0;
  return $result;
}

my @shell_escape_chars = (
  '"', '!', '#', '&', ';', '`', '|',    '*',
  '?', '~', '<', '>', '^', '(', ')',    '[',
  ']', '{', '}', '$', ',', ' ', '\x0A', '\xFF'
);

sub unescape_for_shell {
  my $str = shift;
  if ( !index( $str, '\\\\' ) ) {
    return $str;
  }
  foreach my $c (@shell_escape_chars) {
    my $x       = quotemeta($c);
    my $pattern = "\\\\(" . $x . ")";
    $str =~ s/$pattern/$1/g;
  }
  return $str;
}

sub escape_for_shell {
  my $str = shift;
  my $ret = "";
  foreach my $c ( split //, $str ) {
    my $x      = $c;
    my $escape = 0;
    foreach my $e (@shell_escape_chars) {
      if ( $e eq $x ) {
        $escape = 1;
        last;
      }
    }
    if ( $x eq "'" ) {
      $x =~ s/'/'\\''/;
    }
    if ( $x eq "\\" ) {
      $x = "\\\\";
    }
    if ($escape) {
      $x = "\\" . $x;
    }
    $ret .= "$x";
  }
  $ret = "'" . $ret . "'";
  return $ret;
}

sub escape_for_mysql_command {
  my $str = shift;
  my $ret = "";
  foreach my $c ( split //, $str ) {
    my $x = $c;
    if ( $x eq "'" ) {
      $x =~ s/'/'\\''/;
    }
    $ret .= $x;
  }
  $ret = "'" . $ret . "'";
  return $ret;
}

sub client_cli_prefix {
  my ( $exe, $bindir, $libdir ) = @_;
  croak "unexpected client binary $exe\n" unless $exe =~ /^mysql(?:binlog)?$/;
  my %env = ( LD_LIBRARY_PATH => $libdir );
  my $cli = $exe;
  if ($bindir) {
    if ( ref $bindir eq "ARRAY" ) {
      $env{'PATH'} = $bindir;
    }
    elsif ( ref $bindir eq "" ) {
      $cli = escape_for_shell("$bindir/$exe");
    }
  }
  for my $k ( keys %env ) {
    if ( my $v = $env{$k} ) {
      my @dirs = ref $v eq "ARRAY" ? @{$v} : ( ref $v eq "" ? ($v) : () );
      @dirs = grep { $_ && !/:/ } @dirs;
      if (@dirs) {
        $cli = "$k="
          . join( ":", ( map { escape_for_shell($_) } @dirs ), "\$$k" )
          . " $cli";
      }
    }
  }

  # $cli .= " --no-defaults";
  return $cli;
}

1;
