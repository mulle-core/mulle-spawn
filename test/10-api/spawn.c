/*
 * spawn.c -- mulle-spawn's API test.
 *
 * Every case runs *this* program again with an argument that makes it behave a
 * certain way, rather than reaching for /bin/sh and /bin/echo. That is the point
 * of the API, and it is also what makes the test mean the same thing on Linux,
 * macOS and Windows -- where /bin/sh is not a path that exists.
 *
 * stdout is the result as well as the record: mulle-test compares it against
 * spawn.stdout, or spawn.stdout.<platform> where the platforms genuinely differ
 * (the signal case is one). So nothing that varies run to run or machine to
 * machine may be printed by default -- those lines are behind --verbose, which
 * is what to pass when a check fails and the numbers are what you want:
 *
 *    ./spawn              # exactly the recorded output
 *    ./spawn --verbose    # that, plus timings and the self path
 *
 * To re-record after an intended change, build and run it, read the output, and
 * only then save it. On Windows the tree needs -I<mulle-core>/dlfcn-win32:
 *
 *   cc -std=gnu11 -isystem <mulle-core>/src -o spawn spawn.c \
 *      -L<build>/mulle-core -lmulle-core
 *   ./spawn > spawn.stdout
 */
#include <mulle-spawn/mulle-spawn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
# include <signal.h>
# include <unistd.h>
# define smoke_read(fd, buf, n)    read( fd, buf, n)
# define smoke_write(fd, buf, n)   write( fd, buf, n)
# define smoke_close(fd)           close( fd)
#else
# include <fcntl.h>
# include <io.h>
# define smoke_read(fd, buf, n)    _read( fd, buf, n)
# define smoke_write(fd, buf, n)   _write( fd, buf, n)
# define smoke_close(fd)           _close( fd)
#endif


/*
 * The signal case makes this program die by SIGSEGV on purpose. The sanitizers
 * install a SIGSEGV handler of their own and turn that into an ordinary exit,
 * so the parent would see `exited` where a plain build sees `signalled` -- the
 * assertion would fail for a reason that has nothing to do with mulle-spawn.
 * Tell them to keep their hands off; this crash is the test.
 */
#if defined( __SANITIZE_ADDRESS__)
# define SMOKE_SANITIZER   1
#elif defined( __has_feature)
# if __has_feature( address_sanitizer)
#  define SMOKE_SANITIZER   1
# endif
#endif

#ifdef SMOKE_SANITIZER
const char   *__asan_default_options( void)
{
   return( "handle_segv=0");
}
#endif


static int    failures;
static int    verbose;
static char   self_path[ 4096];
static int    self_found;


/*
 * The tail of a path, for both separators. Used so the self path assertions
 * describe *this* program whatever it happens to be called on disk, instead of
 * hardcoding the name this file was compiled under.
 */
static char   *smoke_basename( char *path)
{
   char   *p;
   char   *slash;

   slash = NULL;
   for( p = path; *p; p++)
      if( *p == '/' || *p == '\\')
         slash = p;

   return( slash ? slash + 1 : path);
}


static void   check( const char *what, int ok)
{
   printf( "%-4s %s\n", ok ? "ok" : "FAIL", what);
   if( ! ok)
      failures++;
}


static void   show( const char *label, struct mulle_spawn_result *r)
{
   printf( "     %s: status=%d exit=%d signal=%d length=%zu output=<%s>\n",
           label, r->status, r->exit_code, r->signal_number, r->length,
           r->output ? r->output : "");
}


/*
 * `dst` must have room for want + 1. A pipe can hand back a short read, so this
 * loops until it has the bytes or the writer is gone.
 */
static int   smoke_read_up_to( int fd, char *dst, size_t want)
{
   size_t   got;
   int      n;

   got = 0;
   while( got < want)
   {
      n = (int) smoke_read( fd, &dst[ got], want - got);
      if( n <= 0)
         break;
      got += (size_t) n;
   }
   dst[ got] = 0;
   return( (int) got);
}


// mulle-core's own monotonic clock, so this runs where there is no
// clock_gettime (mingw) exactly as it does where there is.
//
static double   now_seconds( void)
{
   return( mulle_relativetime_now());
}


/*
 * The child modes. Each one exists so the parent has a real second process to
 * observe, and each exercises a different status: a clean exit, a non zero exit,
 * and one that outlives its deadline after having said something.
 */
static int   child_main( int argc, char **argv)
{
#ifdef _WIN32
   /*
    * A CRT writes \r\n for every \n in a text mode stream, and that translation
    * happens inside the child -- mulle-spawn cannot reach it, which is why
    * mulle_spawn_run undoes the doubling for what it captures. A child that
    * speaks a protocol over a pipe has to say so itself, and it has to be the
    * child: without these three lines the same child reads differently here
    * than it does on POSIX.
    */
   (void) _setmode( _fileno( stdin), _O_BINARY);
   (void) _setmode( _fileno( stdout), _O_BINARY);
   (void) _setmode( _fileno( stderr), _O_BINARY);
#endif

   if( argc >= 2 && ! strcmp( argv[ 1], "--filter"))
   {
      char   line[ 256];

      /* answers what it is given, line by line, until its stdin closes */
      while( fgets( line, sizeof( line), stdin))
      {
         printf( "got %s", line);
         fflush( stdout);
      }
      return( 0);
   }

   if( argc >= 3 && ! strcmp( argv[ 1], "--banner"))
   {
      /* says where it is, then keeps running: the server pattern */
      fprintf( stderr, "listening on 127.0.0.1:%s\n", argv[ 2]);
      fflush( stderr);
      mulle_relativetime_sleep( 30);
      return( 0);
   }

   if( argc >= 3 && ! strcmp( argv[ 1], "--child"))
   {
      printf( "child says %s", argv[ 2]);
      return( 0);
   }

   if( argc >= 3 && ! strcmp( argv[ 1], "--echo"))
   {
      printf( "%s\n", argv[ 2]);
      return( 0);
   }

   if( argc >= 3 && ! strcmp( argv[ 1], "--exit"))
      return( atoi( argv[ 2]));

   if( argc >= 3 && ! strcmp( argv[ 1], "--stall"))
   {
      printf( "out\n");
      fflush( stdout);
      mulle_relativetime_sleep( atoi( argv[ 2]));
      return( 0);
   }

#ifndef _WIN32
   if( ! strcmp( argv[ 1], "--segv"))
   {
      kill( getpid(), SIGSEGV);
      return( 0);
   }
#endif

   return( -1);
}


static void   test_echo( void)
{
   struct mulle_spawn_result   r;
   char                        *argv[ 4];
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--echo";
   argv[ 2] = "hello";
   argv[ 3] = NULL;

   mulle_spawn_result_init( &r, NULL);
   rval = mulle_spawn_run( argv, 5.0, &r);

   check( "echo: run returns 0", rval == 0);
   check( "echo: exited", r.status == mulle_spawn_exited);
   check( "echo: exit code 0", r.exit_code == 0);
   check( "echo: captured stdout", r.output && ! strcmp( r.output, "hello\n"));
   show( "echo", &r);
   mulle_spawn_result_done( &r);
}


static void   test_timeout( void)
{
   struct mulle_spawn_result   r;
   char                        *argv[ 4];
   double                      elapsed;
   double                      start;
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--stall";
   argv[ 2] = "30";
   argv[ 3] = NULL;

   mulle_spawn_result_init( &r, NULL);
   start  = now_seconds();
   rval   = mulle_spawn_run( argv, 0.4, &r);
   elapsed = now_seconds() - start;

   check( "timeout: run returns 0", rval == 0);
   check( "timeout: terminated", r.status == mulle_spawn_terminated);
   check( "timeout: the deadline is enforced", elapsed < 3.0);
   check( "timeout: output survives the kill", r.output && ! strcmp( r.output, "out\n"));
   if( verbose)
      printf( "     deadline fired after %.2fs\n", elapsed);
   show( "timeout", &r);
   mulle_spawn_result_done( &r);
}


static void   test_cannot_run( void)
{
   struct mulle_spawn_result   r;
   char                        *argv[ 2];
   int                         rval;

   argv[ 0] = "/nonexistent/mulle-spawn-smoke-program";
   argv[ 1] = NULL;

   mulle_spawn_result_init( &r, NULL);
   rval = mulle_spawn_run( argv, 5.0, &r);

   check( "cannot run: run returns -1", rval == -1);
   check( "cannot run: status failed", r.status == mulle_spawn_failed);
   show( "cannot run", &r);
   mulle_spawn_result_done( &r);
}


static void   test_exit_code( void)
{
   struct mulle_spawn_result   r;
   char                        *argv[ 4];
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--exit";
   argv[ 2] = "3";
   argv[ 3] = NULL;

   mulle_spawn_result_init( &r, NULL);
   rval = mulle_spawn_run( argv, 5.0, &r);

   check( "exit 3: run returns 0", rval == 0);
   check( "exit 3: exited", r.status == mulle_spawn_exited);
   check( "exit 3: exit code 3", r.exit_code == 3);
   check( "exit 3: no output", r.length == 0 && r.output == NULL);
   show( "exit 3", &r);
   mulle_spawn_result_done( &r);
}


/*
 * signal_number is POSIX by definition -- mulle-spawn.h says so -- so there is
 * nothing to assert on Windows, where a crashing child is just a non zero exit
 * code and the process did come back.
 */
static void   test_signal( void)
{
#ifndef _WIN32
   struct mulle_spawn_result   r;
   char                        *argv[ 3];
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--segv";
   argv[ 2] = NULL;

   mulle_spawn_result_init( &r, NULL);
   rval = mulle_spawn_run( argv, 5.0, &r);

   check( "signal: run returns 0", rval == 0);
   check( "signal: signalled", r.status == mulle_spawn_signalled);
   check( "signal: SIGSEGV recorded", r.signal_number == 11);
   show( "signal", &r);
   mulle_spawn_result_done( &r);
#else
   printf( "skip no POSIX signal_number on this platform\n");
#endif
}


/*
 * The pattern hashspace needs: find yourself, and run yourself again with
 * arguments. Nothing is inherited, so this is a genuine second process.
 */
static void   test_reexec( void)
{
   struct mulle_spawn_result   r;
   char                        *argv[ 4];
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--child";
   argv[ 2] = "smoke";
   argv[ 3] = NULL;

   mulle_spawn_result_init( &r, NULL);
   rval = mulle_spawn_run( argv, 5.0, &r);

   check( "re-exec: run returns 0", rval == 0);
   check( "re-exec: exited", r.status == mulle_spawn_exited);
   check( "re-exec: the child's report came back",
          r.output && ! strcmp( r.output, "child says smoke"));
   show( "re-exec", &r);
   mulle_spawn_result_done( &r);
}


/*
 * The two way case, which is what a stdio protocol needs: the child's stdin and
 * stdout are both pipes, and the parent holds both ends.
 */
static void   test_handle_filter( void)
{
   struct mulle_spawn_handle   h;
   char                        buffer[ 64];
   char                        *argv[ 3];
   int                         n;
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--filter";
   argv[ 2] = NULL;

   rval = mulle_spawn_open( &h, argv, MULLE_SPAWN_PIPE, MULLE_SPAWN_PIPE,
                            MULLE_SPAWN_INHERIT);
   check( "handle: open returns 0", rval == 0);
   if( rval)
      return;

   check( "handle: both ends came back", h.in >= 0 && h.out >= 0);
   check( "handle: stderr was left inherited", h.err == -1);
   check( "handle: it has a pid", h.pid != 0);

   check( "handle: write reaches it", smoke_write( h.in, "ping\n", 5) == 5);

   n = smoke_read_up_to( h.out, buffer, 9);
   check( "handle: and the answer comes back",
          n == 9 && ! strcmp( buffer, "got ping\n"));
   printf( "     handle: the child said <%s>\n", buffer);

   /*
    * Closing our end of its stdin is how a filter is told there is no more.
    * The handle is told by setting the field to -1, so that close does not do
    * it again -- descriptor numbers get reused.
    */
   smoke_close( h.in);
   h.in = -1;

   check( "handle: wait sees it stop", mulle_spawn_wait( &h, 5.0, NULL) == 1);
   check( "handle: close returns 0", mulle_spawn_close( &h, NULL) == 0);
   check( "handle: close resets it",
          h.in == -1 && h.out == -1 && h.err == -1 && h.pid == 0);
}


/*
 * The banner pattern: learn something from the child's stderr while it keeps
 * running, then stop it. `mredis_spawn_server` is exactly this.
 */
static void   test_handle_banner( void)
{
   struct mulle_spawn_handle   h;
   struct mulle_spawn_result   r;
   char                        buffer[ 128];
   char                        *argv[ 4];
   int                         n;
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--banner";
   argv[ 2] = "6379";
   argv[ 3] = NULL;

   mulle_spawn_result_init( &r, NULL);

   rval = mulle_spawn_open( &h, argv, MULLE_SPAWN_INHERIT, MULLE_SPAWN_DISCARD,
                            MULLE_SPAWN_PIPE);
   check( "banner: open returns 0", rval == 0);
   if( rval)
      return;

   check( "banner: stdout was discarded", h.out == -1);
   check( "banner: stderr came back", h.err >= 0);

   n = smoke_read_up_to( h.err, buffer, 28);
   check( "banner: it announced itself",
          n == 28 && ! strcmp( buffer, "listening on 127.0.0.1:6379\n"));
   printf( "     banner: <%s>\n", buffer);

   /* the whole point: the banner arrived while the child is still there */
   check( "banner: it is still running", mulle_spawn_wait( &h, 0.0, NULL) == 0);
   check( "banner: terminate asks it to stop", mulle_spawn_terminate( &h) == 0);
   check( "banner: and it goes", mulle_spawn_wait( &h, 5.0, &r) == 1);

#ifndef _WIN32
   /* there is no SIGTERM to send here, so on POSIX that is what it is */
   check( "banner: stopped by SIGTERM",
          r.status == mulle_spawn_signalled && r.signal_number == 15);
   printf( "     banner: status=%d signal=%d\n", r.status, r.signal_number);
#else
   check( "banner: stopped by TerminateProcess",
          r.status == mulle_spawn_exited && r.exit_code == 1);
   printf( "     banner: status=%d exit=%d\n", r.status, r.exit_code);
#endif

   check( "banner: close returns 0", mulle_spawn_close( &h, NULL) == 0);
   mulle_spawn_result_done( &r);
}


/*
 * A child that is still going when the handle is closed has to go with it, and
 * close is where that is decided -- not wait, which is the polite half.
 */
static void   test_handle_close_kills( void)
{
   struct mulle_spawn_handle   h;
   struct mulle_spawn_result   r;
   char                        buffer[ 64];
   char                        *argv[ 4];
   int                         n;
   int                         rval;

   argv[ 0] = self_path;
   argv[ 1] = "--stall";
   argv[ 2] = "30";
   argv[ 3] = NULL;

   mulle_spawn_result_init( &r, NULL);

   rval = mulle_spawn_open( &h, argv, MULLE_SPAWN_INHERIT, MULLE_SPAWN_PIPE,
                            MULLE_SPAWN_INHERIT);
   check( "close: open returns 0", rval == 0);
   if( rval)
      return;

   n = smoke_read_up_to( h.out, buffer, 4);
   check( "close: it said its piece", n == 4 && ! strcmp( buffer, "out\n"));
   check( "close: and it is still there", mulle_spawn_wait( &h, 0.0, NULL) == 0);

   check( "close: close returns 0", mulle_spawn_close( &h, &r) == 0);
   check( "close: and calls it terminated", r.status == mulle_spawn_terminated);

   mulle_spawn_result_done( &r);
}


/*
 * A program that cannot be run is not a handle to a program that cannot be run.
 */
static void   test_handle_cannot_open( void)
{
   struct mulle_spawn_handle   h;
   char                        *argv[ 2];

   argv[ 0] = "/nonexistent/mulle-spawn-smoke-program";
   argv[ 1] = NULL;

   h.in  = 7;      /* so that a reset is visible as a reset */
   h.out = 7;
   h.err = 7;
   h.pid = 7;

   check( "handle: a program that cannot run is not opened",
          mulle_spawn_open( &h, argv, MULLE_SPAWN_INHERIT, MULLE_SPAWN_PIPE,
                            MULLE_SPAWN_INHERIT) == -1);
   check( "handle: and leaves nothing open",
          h.in == -1 && h.out == -1 && h.err == -1 && h.pid == 0);
}


int   main( int argc, char **argv)
{
   /* the child modes have to come first: they are this program too. --segv
    * takes no argument, so argc is 2 for it and child_main does the checking */
   if( argc >= 2)
   {
      int   rval;

      rval = child_main( argc, argv);
      if( rval != -1)
         return( rval);
   }

#ifdef _WIN32
   /*
    * The record this test is compared against is written with \n on every
    * platform: a CRT would turn every one of ours into \r\n here, and then the
    * Windows output would differ from the Linux and macOS one everywhere
    * instead of only where the platform really behaves differently.
    */
   (void) _setmode( _fileno( stdout), _O_BINARY);
#endif

   verbose = argc >= 2 &&
             ( ! strcmp( argv[ 1], "-v") || ! strcmp( argv[ 1], "--verbose"));

   memset( self_path, 0, sizeof( self_path));
   self_found = mulle_spawn_get_executable_path( self_path, sizeof( self_path)) > 0;

   check( "self path: found", self_found);
   check( "self path: names this program",
          self_found &&
          ! strcmp( smoke_basename( self_path), smoke_basename( argv[ 0])));
   if( verbose)
      printf( "     self path: %s\n", self_path);

   if( ! self_found)
      return( 1);

   test_echo();
   test_timeout();
   test_cannot_run();
   test_exit_code();
   test_signal();
   test_reexec();

   test_handle_filter();
   test_handle_banner();
   test_handle_close_kills();
   test_handle_cannot_open();

   printf( "\n%s (%d failures)\n", failures ? "FAILED" : "all mulle-spawn tests passed",
           failures);
   return( failures ? 1 : 0);
}
