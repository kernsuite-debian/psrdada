/* $Source$
   $Revision$
   $Date$
   $Author$ */

#ifndef DADA_DEAMON_H
#define DADA_DEAMON_H

#ifdef __cplusplus
extern "C" {
#endif

  /* turn the calling process into a daemon */
  void be_a_daemon ();
  int be_a_daemon_with_log(char * logfile);

#ifdef __cplusplus
}
#endif

#endif

