#!/usr/bin/env perl

##############################################################################
#  
#     Copyright (C) 2013 by Andrew Jameson
#     Licensed under the Academic Free License version 2.1
# 
###############################################################################
#
# client_mopsr_bf_mux_recv.pl 
#
# process a single channel of TS data
# 
###############################################################################

use lib $ENV{"DADA_ROOT"}."/bin";

use IO::Socket;
use Getopt::Std;
use File::Basename;
use Mopsr;
use strict;
use threads;
use threads::shared;


sub usage() 
{
  print "Usage: ".basename($0)." BF_ID\n";
}

#
# Global Variables
#
our $dl : shared;
our $quit_daemon : shared;
our $daemon_name : shared;
our %cfg : shared;
our $localhost : shared;
our $bf_id : shared;
our @db_keys : shared;
our @binaries : shared;
our $log_host;
our $sys_log_port;
our $src_log_port;
our $sys_log_sock;
our $src_log_sock;
our $sys_log_file;
our $src_log_file;

#
# Initialize globals
#
$dl = 1;
$quit_daemon = 0;
$daemon_name = Dada::daemonBaseName($0);
%cfg = Mopsr::getConfig("bf");
$bf_id = -1;
@db_keys = ();
@binaries = ();
$localhost = Dada::getHostMachineName(); 
$log_host = $cfg{"SERVER_HOST"};
$sys_log_port = $cfg{"SERVER_BF_SYS_LOG_PORT"};
$src_log_port = $cfg{"SERVER_BF_SRC_LOG_PORT"};
$sys_log_sock = 0;
$src_log_sock = 0;
$sys_log_file = "";
$src_log_file = "";


# Check command line argument
if ($#ARGV != 0)
{
  usage();
  exit(1);
}

$bf_id  = $ARGV[0];

# ensure that our bf_id is valid 
if (($bf_id >= 0) &&  ($bf_id < $cfg{"NUM_BF"}))
{
  # and matches configured hostname
  if ($cfg{"BF_".$bf_id} ne Dada::getHostMachineName())
  {
    print STDERR "BF_".$bf_id." did not match configured hostname [".Dada::getHostMachineName()."]\n";
    usage();
    exit(1);
  }
}
else
{
  print STDERR "bf_id was not a valid integer between 0 and ".($cfg{"NUM_BF"}-1)."\n";
  usage();
  exit(1);
}

#
# Sanity check to prevent multiple copies of this daemon running
#
Dada::preventDuplicateDaemon(basename($0)." ".$bf_id);

###############################################################################
#
# Main
#
{
  # Register signal handlers
  $SIG{INT} = \&sigHandle;
  $SIG{TERM} = \&sigHandle;
  $SIG{PIPE} = \&sigPipeHandle;

  $sys_log_file = $cfg{"CLIENT_LOG_DIR"}."/".$daemon_name."_".$bf_id.".log";
  $src_log_file = $cfg{"CLIENT_LOG_DIR"}."/".$daemon_name."_".$bf_id.".src.log";
  my $pid_file =  $cfg{"CLIENT_CONTROL_DIR"}."/".$daemon_name."_".$bf_id.".pid";

  # this is data stream we will be reading from

  # Autoflush STDOUT
  $| = 1;

  # become a daemon
  Dada::daemonize($sys_log_file, $pid_file);

  # Open a connection to the server_sys_monitor.pl script
  $sys_log_sock = Dada::nexusLogOpen($log_host, $sys_log_port);
  if (!$sys_log_sock) {
    print STDERR "Could open sys log port: ".$log_host.":".$sys_log_port."\n";
  }

  $src_log_sock = Dada::nexusLogOpen($log_host, $src_log_port);
  if (!$src_log_sock) {
    print STDERR "Could open src log port: ".$log_host.":".$src_log_port."\n";
  }

  msg (0, "INFO", "STARTING SCRIPT");

  my $control_thread = threads->new(\&controlThread, $pid_file);

  my @tb_threads = ();
  my $num_tbs = $cfg{"NUM_TIED_BEAMS"};
  my $i;
  for ($i=0; $i<$num_tbs; $i++)
  {
    my $db_key = Dada::getDBKey($cfg{"DATA_BLOCK_PREFIX"}, $bf_id, $cfg{"NUM_BF"}, 
                                $cfg{"TIED_BEAM_".$i."_DATA_BLOCK"});
    push (@db_keys, $db_key);
    push (@binaries, "");
    push (@tb_threads, threads->new(\&tbThread, $i, $db_key));
  }

  while (!$quit_daemon)
  {
    sleep(1); 
  }

  # Rejoin our daemon control thread
  msg(2, "INFO", "joining control thread");
  $control_thread->join();

  for ($i=0; $i<$num_tbs; $i++)
  {
    msg(2, "INFO", "joining TB thread ".$i);
    $tb_threads[$i]->join();
  }

  msg(0, "INFO", "STOPPING SCRIPT");

  # Close the nexus logging connection
  Dada::nexusLogClose($sys_log_sock);

  exit (0);
}

sub tbThread($$)
{
  my ($tb_id, $db_key) = @_;

  my ($cmd, $result, $response, $raw_header, $full_cmd, $proc_cmd_file);
  my ($proc_cmd, $proc_dir, $bf_dir);

  $bf_dir  = "BF".sprintf("%02d", $bf_id);

  # continuously run mopsr_dbib for this PWC
  while (!$quit_daemon)
  {
    $cmd = "dada_header -k ".$db_key;
    msg(2, "INFO", "main: ".$cmd);
    $raw_header = `$cmd 2>&1`;
    msg(2, "INFO", "main: ".$cmd." returned");

    if ($? != 0)
    {
      if ($quit_daemon)
      {
        msg(2, "INFO", "dada_header failed, but quit_daemon true");
      }
      else
      {
        msg(0, "ERROR", "dada_header failed: ".$raw_header);
        $quit_daemon = 1;
      }
    }
    else
    {
      my %header = Dada::headerToHash($raw_header);
      msg (2, "INFO", "UTC_START=".$header{"UTC_START"}." NCHAN=".$header{"NCHAN"}." NANT=".$header{"NANT"});

      # by default discard the current observation
      $proc_cmd = "dada_dbnull -k ".$db_key." -z -s";

      # Add the dada header file to the proc_cmd
      $proc_cmd_file = $cfg{"CONFIG_DIR"}."/".$header{"PROC_FILE"};

      msg(1, "INFO", "Full path to PROC_FILE: ".$proc_cmd_file);

      if (-f $proc_cmd_file)
      {
        if ($cfg{"BF_STATE_".$bf_id} eq "active")
        {
          my %proc_cmd_hash = Dada::readCFGFile($proc_cmd_file);
          $proc_cmd = $proc_cmd_hash{"PROC_CMD"};
        }
        else
        {
          msg(0, "INFO", "BF_STATE_".$bf_id." == ".$cfg{"BF_STATE_".$bf_id});
        }
      }
      else
      {
        msg(0, "ERROR", "PROC_FILE did not exist: ".$proc_cmd_file);
      }

      # create a local directory for the output from this channel
      ($result, $response) = createLocalDirs(\%header);
      if ($result ne "ok")
      {
        msg(0, "ERROR", "could not create local dir for bf_id=".$bf_id.": ".$response);
        $proc_cmd = "dada_dbnull -k ".$db_key." -z -s";
        $proc_dir = "/";
      }
      else
      {
        $proc_dir = $response;
        my $obs_header = $proc_dir."/obs.header";
      }

      # replace the SHM key with db_key
      $proc_cmd =~ s/<DADA_KEY>/$db_key/;

      $proc_cmd =~ s/<IN_DADA_KEY>/$db_key/;

      # replace <DADA_RAW_DATA> tag with processing dir
      $proc_cmd =~ s/<DADA_DATA_PATH>/$proc_dir/;

      # replace DADA_UTC_START with actual UTC_START
      $proc_cmd =~ s/<DADA_UTC_START>/$header{"UTC_START"}/;

      # replace DADA_GPU_ID with actual GPU_ID 
      $proc_cmd =~ s/<DADA_GPU_ID>/$cfg{"BF_GPU_ID_".$bf_id}/;

      # replace DADA_GPU_ID with actual GPU_ID 
      $proc_cmd =~ s/<DADA_CORE>/$cfg{"BF_CORE_".$bf_id}/;

      # replace DADA_CH_ID with bf_dir
      $proc_cmd =~ s/<DADA_CH_ID>/$bf_dir/;

      # replace <DADA_INFO> tags with the matching input .info file
      my $tmp_info_file = "";
      if ($proc_cmd =~ m/<DADA_INFO>/)
      {
        $tmp_info_file =  "/tmp/mopsr_".$db_key.".info";
        # ensure a file exists with the write processing key
        if (! -f $tmp_info_file)
        {
          open FH, ">".$tmp_info_file;
          print FH "DADA INFO:\n";
          print FH "key ".$db_key."\n";
          close FH;
        }
        $proc_cmd =~ s/<DADA_INFO>/$tmp_info_file/;
      }

      my ($binary, $junk) = split(/ /,$proc_cmd, 2);
      $cmd = "ls -l ".$cfg{"SCRIPTS_DIR"}."/".$binary;
      ($result, $response) = Dada::mySystem($cmd);
      msg(2, "INFO", "main: ".$cmd.": ".$result." ".$response);

      if ($binary eq "dspsr")
      {
        $proc_cmd = "export T2USER=".$cfg{"USER"}.$bf_id.$tb_id."; ".$proc_cmd;

        # need to ensure that the sub-integration length is at least twice the 
        # folding period
        $cmd = "psrcat -x -all -c P0 ".$header{"SOURCE"}." | awk '{print \$1}'";
        ($result, $response) = Dada::mySystem($cmd);
        msg(2, "INFO", "main: ".$cmd.": ".$result." ".$response);
      }

      $binaries[$tb_id] = $binary." ".$tmp_info_file;
      $cmd = "cd ".$proc_dir."; ".$proc_cmd;
      msg(1, "INFO", "START ".$proc_cmd);
      ($result, $response) = Dada::mySystemPiped($cmd, $src_log_file, $src_log_sock, 
                                                 "src", sprintf("%02d",$bf_id), $daemon_name, "bf_tbs");
      msg(1, "INFO", "END   ".$proc_cmd);
      $binaries[$tb_id] = "";
      if ($result ne "ok")
      {
        $quit_daemon = 1;
        if ($result ne "ok")
        {
          msg(0, "ERROR", $cmd." failed: ".$response);
        }
      }
    }
  }

}

#
# Create the local directories required for this observation
#
sub createLocalDirs(\%)
{
  my ($h_ref) = @_;

  msg(2, "INFO", "createLocalDirs()");

  my %h = %$h_ref;
  my $utc_start = $h{"UTC_START"};
  my $source = $h{"SOURCE"};
  my $bf_dir  = "BF".sprintf("%02d", $bf_id);
  my $dir   = $cfg{"CLIENT_RESULTS_DIR"}."/".$bf_dir."/".$utc_start."/".$source;

  my ($cmd, $result, $response);

  msg(2, "INFO", "createLocalDirs: mkdirRecursive(".$dir.", 0755)");
  ($result, $response) = Dada::mkdirRecursive($dir, 0755);
  msg(3, "INFO", "createLocalDirs: ".$result." ".$response);
  if ($result ne "ok")
  {
    return ("fail", "Could not create local dir: ".$response);
  }

  # create an obs.header file in the processing dir:
  msg(2, "INFO", "createLocalDirs: creating obs.header");
  my $file = $dir."/obs.header";
  open(FH,">".$file.".tmp");
  my $k = "";
  foreach $k ( keys %h)
  {
    print FH Dada::headerFormat($k, $h{$k})."\n";
  }
  close FH;
  rename($file.".tmp", $file);

  return ("ok", $dir);
}

#
# Logs a message to the nexus logger and print to STDOUT with timestamp
#
sub msg($$$)
{
  my ($level, $type, $msg) = @_;

  if ($level <= $dl)
  {
    my $time = Dada::getCurrentDadaTime();
    if (!($sys_log_sock)) {
      $sys_log_sock = Dada::nexusLogOpen($log_host, $sys_log_port);
    }
    if ($sys_log_sock) {
      Dada::nexusLogMessage($sys_log_sock, sprintf("%02d",$bf_id), $time, "sys", $type, "bfproc", $msg);
    }
    print "[".$time."] ".$msg."\n";
  }
}

sub controlThread($)
{
  (my $pid_file) = @_;

  msg(2, "INFO", "controlThread : starting");

  my $host_quit_file = $cfg{"CLIENT_CONTROL_DIR"}."/".$daemon_name.".quit";
  my $pwc_quit_file  = $cfg{"CLIENT_CONTROL_DIR"}."/".$daemon_name."_".$bf_id.".quit";

  while ((!$quit_daemon) && (!(-f $host_quit_file)) && (!(-f $pwc_quit_file)))
  {
    sleep(1);
  }

  $quit_daemon = 1;

  my ($cmd, $result, $response, $i, $db_key);

  for ($i=0; $i<$cfg{"NUM_TIED_BEAMS"}; $i++)
  {
    $db_key = $db_keys[$i];
    
    $cmd = "^dada_header -k ".$db_key;
    msg(2, "INFO" ,"controlThread: killProcess(".$cmd.", mpsr)");
    ($result, $response) = Dada::killProcess($cmd, "mpsr");
    msg(3, "INFO" ,"controlThread: killProcess() ".$result." ".$response);

    $cmd = "^dada_dbnull -k ".$db_key;
    msg(2, "INFO" ,"controlThread: killProcess(".$cmd.", mpsr)");
    ($result, $response) = Dada::killProcess($cmd, "mpsr");
    msg(3, "INFO" ,"controlThread: killProcess() ".$result." ".$response);

    if ($binaries[$i] ne "")
    {
      $cmd = "^".$binaries[$i];
      msg(2, "INFO" ,"controlThread: killProcess(".$cmd.", mpsr)");
      ($result, $response) = Dada::killProcess($cmd, "mpsr");
      msg(3, "INFO" ,"controlThread: killProcess() ".$result." ".$response);
    }
  }

  if ( -f $pid_file) {
    msg(2, "INFO", "controlThread: unlinking PID file");
    unlink($pid_file);
  } else {
    msg(1, "WARN", "controlThread: PID file did not exist on script exit");
  }

  msg(2, "INFO", "controlThread: exiting");

}

sub sigHandle($)
{
  my $sigName = shift;
  print STDERR $daemon_name." : Received SIG".$sigName."\n";

  # if we CTRL+C twice, just hard exit
  if ($quit_daemon) {
    print STDERR $daemon_name." : Recevied 2 signals, Exiting\n";
    exit 1;

  # Tell threads to try and quit
  } else {

    $quit_daemon = 1;
    if ($sys_log_sock) {
      close($sys_log_sock);
    }
  }
}

sub sigPipeHandle($)
{
  my $sigName = shift;
  print STDERR $daemon_name." : Received SIG".$sigName."\n";
  $sys_log_sock = 0;
  if ($log_host && $sys_log_port) {
    $sys_log_sock = Dada::nexusLogOpen($log_host, $sys_log_port);
  }
}

