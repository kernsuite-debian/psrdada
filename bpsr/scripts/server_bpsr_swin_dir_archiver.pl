#!/usr/bin/env perl

use lib $ENV{"DADA_ROOT"}."/bin";

use strict;
use warnings;
use Bpsr;
use Dada::server_general_dir_archiver qw(%cfg);

#
# Global Variable Declarations
#
%cfg = Bpsr::getConfig();

sub usage() {
  print STDERR "Usage: ".$0."\n";
}

#
# Initialize module variables
#
$Dada::server_general_dir_archiver::dl = 1;
$Dada::server_general_dir_archiver::daemon_name = Dada::daemonBaseName($0);
$Dada::server_general_dir_archiver::robot = 1;
$Dada::server_general_dir_archiver::drive_id = 0;
$Dada::server_general_dir_archiver::type = "swin";
$Dada::server_general_dir_archiver::pid = "HML";
$Dada::server_general_dir_archiver::required_host = "tapeserv01.hpc.swin.edu.au";

# Autoflush STDOUT
$| = 1;

if ($#ARGV != -1) {
  usage();
  exit(1);
}

my $result = 0;

$result = Dada::server_general_dir_archiver->main();

exit($result);

