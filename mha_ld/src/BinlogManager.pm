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

package MHA::BinlogManager;

use strict;
use warnings FATAL => 'all';

use English qw(-no_match_vars);
use Carp qw(croak);
use MHA::BinlogHeaderParser;
use MHA::NodeUtil;
use File::Basename;
use File::Copy;
use File::Path;
use Errno();

sub new {
  my $class = shift;
  my $self  = {
    mysql_version       => undef,
    handle_raw_binlog   => undef,
    disable_log_bin     => undef,
    relay_log_info      => undef,
    mysqlbinlog_version => undef,
    dir                 => undef,
    client_bindir       => undef,
    client_libdir       => undef,
    prefix              => undef,
    cur_log             => undef,
    end_log             => undef,
    end_num             => undef,
    debug               => undef,
    @_,
  };
  $self->{mysqlbinlog} = MHA::NodeUtil::client_cli_prefix(
    "mysqlbinlog",
    $self->{client_bindir},
    $self->{client_libdir},
  );
  return bless $self, $class;
}

sub get_apply_arg($) {
  my $self = shift;
  my $arg  = "";
  if (
    should_suppress_row_format(
      $self->{mysqlbinlog_version}, $self->{mysql_version},
      $self->{mysqlbinlog}
    )
    )
  {
    $arg .= " --base64-output=never";
  }
  if ( $self->{disable_log_bin} ) {
    $arg .= " --disable-log-bin";
  }
  return $arg;
}

sub parse_version($) {
  my $str = shift;
  my $result = sprintf( '%03d%03d', $str =~ m/(\d+)/g );
  return $result;
}

sub mysqlbinlog_version_ge {
  my ( $my_version, $target_version ) = @_;
  my $result =
    parse_version($my_version) ge parse_version($target_version) ? 1 : 0;
  return $result;
}

sub die_if_too_old_version {
  my $mysqlbinlog_version = shift;
  my $mysqlbinlog         = shift;
  if ( !mysqlbinlog_ge_50($mysqlbinlog_version) ) {
    croak
"$mysqlbinlog version is $mysqlbinlog_version. This is too old. MHA supports MySQL version 5.0 (mysqlbinlog version 3.2) or higher. Recommended mysqlbinlog version is 3.3+, which is included in MySQL 5.1 or higher.\n";
  }
}

sub init_mysqlbinlog($) {
  my $self = shift;
  eval {
    unless ( $self->{mysql_version} ) {
      croak "mysql version not found.\n";
    }
    my $v = `$self->{mysqlbinlog} --version`;
    my ( $high, $low ) = MHA::NodeUtil::system_rc($?);
    if ( $high || $low ) {
      croak "$self->{mysqlbinlog} version command failed with rc $high:$low, "
        . "please verify PATH, LD_LIBRARY_PATH, and client options\n";
    }
    chomp($v);
    if ( $v =~ /Ver (\d+\.\d+)/ ) {
      $self->{mysqlbinlog_version} = $1;
    }
    croak "$self->{mysqlbinlog} version not found!\n"
      unless ( $self->{mysqlbinlog_version} );
    die_if_too_old_version( $self->{mysqlbinlog_version},
      $self->{mysqlbinlog} );
    if ( !mysqlbinlog_ge_51( $self->{mysqlbinlog_version} ) ) {
      print
"$self->{mysqlbinlog} version is $self->{mysqlbinlog_version} (included in MySQL Client 5.0 or lower). This is not recommended. Consider upgrading MySQL Client to 5.1 or higher.\n";
    }
    if ( !mysqlbinlog_ge_51( $self->{mysqlbinlog_version} )
      && $self->{mysql_version}
      && MHA::NodeUtil::mysql_version_ge( $self->{mysql_version}, "5.1.0" ) )
    {
      croak sprintf(
"$self->{mysqlbinlog} is %s (included in MySQL Client 5.0 or lower), but MySQL server version is %s. mysqlbinlog can not parse row based events. Terminating script for safety reasons.\n",
        $self->{mysqlbinlog_version},
        $self->{mysql_version}
      );
    }
  };
  if ($@) {
    my $e = $@;
    undef $@;
    die $e;
  }
  return;
}

sub open_test($) {
  my $file = shift;
  my $fh;
  open( $fh, "<", $file ) or croak "$!:$file\n";
  close($fh);
}

sub init_from_dir_file($$$) {
  my $self = shift;
  $self->{dir} = shift;
  my $current_binlog = shift;
  $self->{end_log} = get_end_binlog( $current_binlog, $self->{dir} );
  croak "Failed to get tail of the relay/bin log name!\n"
    unless ( $self->{end_log} );
  open_test("$self->{dir}/$self->{end_log}");
  ( $self->{prefix}, $self->{end_num} ) =
    get_head_and_number( $self->{end_log} );
  croak "Failed to get relay/bin log prefix!\n" unless ( $self->{prefix} );
  croak "Failed to get relay/bin log number!\n"
    if ( !defined $self->{end_num} );
}

sub init_from_relay_log_info($$$) {
  my $self = shift;
  $self->{relay_log_info} = shift;
  my $datadir = shift;
  ( $self->{dir}, $self->{end_log}, $self->{cur_log} ) =
    get_relaydir_and_files_from_rinfo( $self->{relay_log_info}, $datadir );
  croak "Failed to get relay log directory!\n" unless ( $self->{dir} );
  croak "Failed to get relay log end file!\n"  unless ( $self->{end_log} );
  open_test("$self->{dir}/$self->{end_log}");
  ( $self->{prefix}, $self->{end_num} ) =
    get_head_and_number( $self->{end_log} );
  croak "Failed to get relay log prefix!\n" unless ( $self->{prefix} );
  croak "Failed to get relay log number!\n" if ( !defined $self->{end_num} );
}

sub mysqlbinlog_ge {
  my $mysqlbinlog_version = shift;
  my $criteria            = shift;
  if ( mysqlbinlog_version_ge( $mysqlbinlog_version, $criteria ) ) {
    return 1;
  }
  else {
    return 0;
  }
}

sub mysqlbinlog_ge_51 {
  my $mysqlbinlog_version = shift;
  return mysqlbinlog_ge( $mysqlbinlog_version, "3.3" );
}

sub mysqlbinlog_ge_50 {
  my $mysqlbinlog_version = shift;
  return mysqlbinlog_ge( $mysqlbinlog_version, "3.2" );
}

# Return: "/var/lib/mysql" "mysqld-relay-bin.000040"
sub get_relaydir_and_filename_from_rinfo($$) {
  my $relay_log_info_path = shift;
  my $datadir             = shift;
  my $fh;
  if ( !open( $fh, "<", $relay_log_info_path ) ) {
    croak "Could not open relay-log-info file $relay_log_info_path.\n";
  }

  my $current_relay_log_file = readline($fh);
  chomp($current_relay_log_file);

  # for 5.6 relay-log.info
  if ( $current_relay_log_file =~ /^[0-9]+$/ ) {
    $current_relay_log_file = readline($fh);
    chomp($current_relay_log_file);
  }
  my $relay_dir = dirname($current_relay_log_file);

  # for compatibility
  unless ($datadir) {
    $datadir = dirname($relay_log_info_path);
  }
  if ( !$current_relay_log_file || !$relay_dir ) {
    croak "Coundln't get current relay log name.\n";
  }

  unless ( $current_relay_log_file =~ m/^\// ) {
    $current_relay_log_file =~ s/^\.\///;
    $datadir =~ s/\/$//;
    $current_relay_log_file = $datadir . "/" . $current_relay_log_file;
    $relay_dir              = $datadir;
  }
  my $relay_log_basename = basename($current_relay_log_file);
  return $relay_dir, $relay_log_basename;
}

sub get_head_and_number($) {
  my $binlog_filename = shift;
  my $log_number      = $binlog_filename;
  $log_number =~ m/(.*)\.([0-9]+)/;
  my $binlog_head = $1;
  $log_number = $2;
  return $binlog_head, $log_number;
}

sub find_correct_binlog_dir($$) {
  my $binlog_file      = shift;
  my $binlog_dirs      = shift;
  my @binlog_dir_array = split( /,/, $binlog_dirs );
  foreach (@binlog_dir_array) {
    return $_ if ( -f "$_/$binlog_file" );
  }
}

sub find_correct_binlog_dir_file_from_prefix($$) {
  my $binlog_prefix    = shift;
  my $binlog_dirs      = shift;
  my @binlog_dir_array = split( /,/, $binlog_dirs );
  foreach (@binlog_dir_array) {
    my @files = get_all_binlogs_from_prefix( $binlog_prefix, $_ );
    return ( $_, $files[0] ) if ( $#files >= 0 );
  }
}

sub get_all_binlogs_from_prefix($$) {
  my $binlog_prefix = shift;
  my $binlog_dir    = shift;
  opendir my $dir, "$binlog_dir";
  my @files =
    grep { m/^$binlog_prefix\.[0-9]+/ } readdir $dir;
  @files = sort @files;
  closedir $dir;
  return @files;
}

sub get_end_binlog_from_prefix($$) {
  my $binlog_prefix = shift;
  my $binlog_dir    = shift;
  my @files = get_all_binlogs_from_prefix( $binlog_prefix, $binlog_dir );
  if ( $#files < 0 ) {

    #No binlog file found!
    return;
  }
  return $files[$#files];
}

sub get_end_binlog($$) {
  my $binlog_filename = shift;
  my $binlog_dir      = shift;
  my ( $prefix, $num ) = get_head_and_number($binlog_filename);
  return get_end_binlog_from_prefix( $prefix, $binlog_dir );
}

sub get_binlog_start_end($$) {
  my $start_binlog_file = shift;
  my $binlog_dir        = shift;
  my ( $binlog_prefix, $start_num ) = get_head_and_number($start_binlog_file);
  my $end_binlog_file =
    get_end_binlog_from_prefix( $binlog_prefix, $binlog_dir );
  croak "Binlog not found from $binlog_dir!\n" unless ($end_binlog_file);
  my ( $tmp, $end_num ) = get_head_and_number($end_binlog_file);
  return ( $binlog_prefix, $start_num, $end_num );
}

sub get_prev_number($) {
  my $log_number = shift;
  $log_number--;
  return sprintf( "%06d", $log_number );
}

sub get_prev_file($) {
  my $log_file = shift;
  $log_file =~ m/(.*)\.([0-9]+)/;
  $log_file = $1 . "." . sprintf( "%06d", ( $2 - 1 ) );
  return $log_file;
}

sub get_prev_number_from_file($) {
  my $log_file = shift;
  my ( $ignore, $number ) = get_head_and_number($log_file);
  return get_prev_number($number);
}

sub get_post_number($) {
  my $log_number = shift;
  $log_number++;
  return sprintf( "%06d", $log_number );
}

sub get_post_file($) {
  my $log_file = shift;
  $log_file =~ m/(.*)\.([0-9]+)/;
  $log_file = $1 . "." . sprintf( "%06d", ( $2 + 1 ) );
  return $log_file;
}

sub should_suppress_row_format {
  my $mysqlbinlog_version = shift;
  my $mysql_version       = shift;
  my $mysqlbinlog         = shift;
  my $suppress_row_format = 0;
  if ( mysqlbinlog_ge_51($mysqlbinlog_version)
    && !MHA::NodeUtil::mysql_version_ge( $mysql_version, "5.1.0" ) )
  {
    print
"$mysqlbinlog is 5.1 or higher, and MySQL version on the target server is 5.0 or lower. So using mysqlbinlog --base64-output=never to disable BINLOG events..\n";
    $suppress_row_format = 1;
  }
  return $suppress_row_format;
}

# dir, read_file, exec_file
sub get_relaydir_and_files_from_rinfo {
  my $rinfo   = shift;
  my $datadir = shift;
  my ( $relay_dir, $exec_relay_file ) =
    get_relaydir_and_filename_from_rinfo( $rinfo, $datadir );
  my $end_relay_file = get_end_binlog( $exec_relay_file, $relay_dir );
  return ( $relay_dir, $end_relay_file, $exec_relay_file );
}

sub is_binlog_head_readable($) {
  my $self = shift;
  my $file = shift;

  # higher than binlog file header (4 bytes)
  return system("$self->{mysqlbinlog} --stop-position=5 $file > /dev/null");
}

sub get_end_binlog_fde($$$) {
  my $self = shift;
  my $dir  = shift;
  my $file = shift;

  my $p = new MHA::BinlogHeaderParser(
    dir   => $dir,
    file  => $file,
    debug => $self->{debug}
  );
  $p->open_binlog();
  return $p->parse_init_headers();
}

sub dump_init_binlog_without_fde($$$$) {
  my $self    = shift;
  my $dir     = shift;
  my $file    = shift;
  my $outfile = shift;
  my $p       = new MHA::BinlogHeaderParser(
    dir   => $dir,
    file  => $file,
    debug => $self->{debug}
  );
  $p->open_binlog();
  print
"  Dumping binlog head events (rotate events), skipping format description events from $dir/$file.. ";
  my $start_effective_pos = $p->parse_init_headers( 1, $outfile );
  print "dumped up to pos $start_effective_pos. ok.\n";
  return $start_effective_pos;
}

sub dump_binlog_header_fde($$$$) {
  my $self     = shift;
  my $dir      = shift;
  my $filebase = shift;
  my $outfile  = shift;

  my $file = "$dir/$filebase";
  my $end_fde_pos = $self->get_end_binlog_fde( $dir, $filebase );

  my $fp;
  my $buf;
  my $out;
  open( $fp, "<", $file ) or croak "$!:$file";
  binmode $fp;
  seek( $fp, 0, 0 );
  read( $fp, $buf, $end_fde_pos );

  open( $out, ">", $outfile ) or croak "$!:$outfile";
  binmode $out;
  print $out $buf;
  close($fp);
  close($out);

  return $end_fde_pos;
}

sub check_first_header_readable($$$) {
  my $fp   = shift;
  my $size = shift;
  my $pos  = shift;
  my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    MHA::BinlogHeaderParser::unpack_header_util( $fp, $size, $pos );
}

sub dump_binlog_from_pos($$$$$) {
  my $self     = shift;
  my $dir      = shift;
  my $filebase = shift;
  my $from_pos = shift;
  my $outfile  = shift;

  my $file = "$dir/$filebase";
  my $size = -s $file;
  my $fp;
  my $out;
  my $buf;

  if ( $from_pos >= $size ) {
    print
"  No need to dump effective binlog data from $file (pos starts $from_pos, filesize $size). Skipping.\n";
    return;
  }
  else {
    print
"  Dumping effective binlog data from $file position $from_pos to tail($size)..";
  }

  open( $fp, "<", $file ) or croak "$!:$file";
  binmode $fp;
  check_first_header_readable( $fp, $size, $from_pos );
  seek( $fp, $from_pos, 0 );
  read( $fp, $buf, $size - $from_pos );
  open( $out, ">>", $outfile ) or croak "$!:$outfile";
  binmode $out;
  print $out $buf;
  close($fp);
  close($out);
  print " ok.\n";
}

sub dump_binlog($$$$$$) {
  my $self          = shift;
  my $dir           = shift;
  my $from_file     = shift;
  my $from_pos      = shift;
  my $out_diff_file = shift;
  my $first_file    = shift;

  my $effective_from_pos;
  if ($first_file) {
    my $end_fde_pos =
      $self->dump_binlog_header_fde( $dir, $from_file, $out_diff_file );
    print
"  Dumping binlog format description event, from position 0 to $end_fde_pos.. ok.\n";
    if ( $from_pos < $end_fde_pos ) {
      $effective_from_pos = $end_fde_pos;
    }
    else {
      $effective_from_pos = $from_pos;
    }
  }
  else {
    $effective_from_pos =
      $self->dump_init_binlog_without_fde( $dir, $from_file, $out_diff_file );
  }
  $self->dump_binlog_from_pos( $dir, $from_file, $effective_from_pos,
    $out_diff_file );
}

sub dump_mysqlbinlog($$$$$$) {
  my $self                = shift;
  my $binlog_dir          = shift;
  my $from_file           = shift;
  my $from_pos            = shift;
  my $out_diff_file       = shift;
  my $suppress_row_format = shift;
  my $disable_log_bin     = $self->{disable_log_bin};

  my $rc;
  my $binlog_file = "$binlog_dir/$from_file";
  my $file_size   = -s $binlog_file;
  if ( $from_pos > 4 && $from_pos > $file_size ) {
    print
"Target file $binlog_file size=$file_size, this is smaller than exec pos $from_pos. Skipping.\n";
    return 1;
  }
  elsif ( $from_pos > 4 && $from_pos == $file_size ) {
    print "No additional binlog events found.\n";
    return 1;
  }
  my $command =
    "echo \"# Binary/Relay log file $from_file started\" >> $out_diff_file";
  system($command);

  $command = "$self->{mysqlbinlog} --start-position=$from_pos ";
  if ($suppress_row_format) {
    $command .= " --base64-output=never";
  }
  if ($disable_log_bin) {
    $command .= " --disable-log-bin";
  }

  $command .= " $binlog_file";
  if ( !$self->{skip_filter} ) {
    $command .= "| filter_mysqlbinlog";
  }
  $command .= " >> $out_diff_file";
  if ( $self->{debug} ) {
    print "Executing command: $command\n";
  }

  $rc = system($command);
  if ($rc) {
    my ( $high, $low ) = MHA::NodeUtil::system_rc($rc);
    croak
"FATAL: $self->{mysqlbinlog} to binlog/relaylog file $from_file, generating to $out_diff_file failed with rc $high:$low!\n";
  }
  return 0;
}

sub concat_all_binlogs_from($$$$) {
  my $self      = shift;
  my $start_log = shift;
  my $start_pos = shift;
  my $outfile   = shift;

  my $binlog_dir          = $self->{dir};
  my $binlog_prefix       = $self->{prefix};
  my $end_num             = $self->{end_num};
  my $handle_raw_binlog   = $self->{handle_raw_binlog};
  my $mysqlbinlog_version = $self->{mysqlbinlog_version};
  my $mysql_version       = $self->{mysql_version};

  my ( $ignore, $start_num ) = get_head_and_number($start_log);

  my $suppress_row_format;
  if ( !$handle_raw_binlog ) {
    $suppress_row_format =
      should_suppress_row_format( $mysqlbinlog_version, $mysql_version,
      $self->{mysqlbinlog} );
  }
  print
" Concat binary/relay logs from $binlog_prefix.$start_num pos $start_pos to $binlog_prefix.$end_num EOF into $outfile ..\n";

  for ( my $i = $start_num ; $i <= $end_num ; $i++ ) {
    my $from_pos   = 4;
    my $first_file = 0;
    my $from_file  = $binlog_prefix . "." . sprintf( "%06d", ($i) );
    if ( $i == $start_num ) {
      $first_file = 1;
      $from_pos   = $start_pos;
    }

    # This should never happen
    if ( !-f "$binlog_dir/$from_file" ) {
      croak "Target file $binlog_dir/$from_file not found!\n";
    }

    if ($handle_raw_binlog) {
      $self->dump_binlog( $self->{dir}, $from_file, $from_pos, $outfile,
        $first_file );
    }
    else {
      my $rc =
        $self->dump_mysqlbinlog( $self->{dir}, $from_file, $from_pos,
        $outfile, $suppress_row_format );
      if ($rc) {
        return $rc;
      }
    }
  }
  if ( -f $outfile ) {
    if ($handle_raw_binlog) {
      croak "$outfile is broken!\n" if $self->is_binlog_head_readable($outfile);
      if ( -s $outfile <=
        $self->get_end_binlog_fde( dirname($outfile), basename($outfile) ) )
      {
        print " $outfile has no effective data events.\n";
        return 2;
      }
    }
    print " Concat succeeded.\n";
    return 0;
  }
  else {
    croak "$outfile not exists!\n";
  }
  return 1;
}

sub concat_generated_binlogs($$$) {
  my $self      = shift;
  my $files_ref = shift;
  my $out       = shift;

  my @diffs = @$files_ref;
  if ( $self->{handle_raw_binlog} ) {
    print " Concat all apply files to $out ..\n";
    my $from_pos   = 0;
    my $first_file = 1;
    foreach my $file (@diffs) {
      if ($first_file) {
        print " Copying the first binlog file $file to $out..";
        copy( $file, $out )
          or croak "File copy from $file to $out failed! $!\n";
        print " ok.\n";
        $first_file = 0;
      }
      else {
        $from_pos =
          $self->dump_init_binlog_without_fde( dirname($file), basename($file),
          $out );
        if ( $from_pos == 0 ) {
          print " $file has no effective binlog events.\n";
          next;
        }
        print " $file has effective binlog events from pos $from_pos.\n";
        $self->dump_binlog_from_pos( dirname($file), basename($file), $from_pos,
          $out );
      }
    }
    if ( $self->is_binlog_head_readable($out) ) {
      croak "$out is broken!\n";
    }
  }
  else {
    croak "not supported.\n";
  }
  print " Concat succeeded.\n";
}

1;

