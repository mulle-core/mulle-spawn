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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
# include <signal.h>
# include <unistd.h>
# include <sys/stat.h>
# define smoke_read(fd, buf, n)    read( fd, buf, n)
# define smoke_write(fd, buf, n)   write( fd, buf, n)
# define smoke_close(fd)           close( fd)
# define smoke_mkdir(path)         mkdir( path, 0755)
# define smoke_rmdir(path)         rmdir( path)
# define smoke_getcwd(buf, n)      getcwd( buf, n)
#else
# include <direct.h>
# include <fcntl.h>
# include <io.h>
# define smoke_read(fd, buf, n)    _read( fd, buf, n)
# define smoke_write(fd, buf, n)   _write( fd, buf, n)
# define smoke_close(fd)           _close( fd)
# define smoke_mkdir(path)         _mkdir( path)
# define smoke_rmdir(path)         _rmdir( path)
# define smoke_getcwd(buf, n)      _getcwd( buf, n)
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
 * Unset and empty are two different things an environment can be, and the first
 * is what a replaced environment produces -- so they are told apart here rather
 * than both printed as nothing.
 */
static void   print_variable( const char *name)
{
   char   *value;

   value = getenv( name);
   printf( "%s=%s\n", name, value ? value : "<unset>");
}


/*
 * The marker goes into the parent's own environment, so that what the child says
 * about it is a statement about the launch and not about whatever environment
 * this test was started in.
 */
static void   set_marker( void)
{
#ifdef _WIN32
   (void) _putenv_s( "MULLE_SPAWN_MARKER", "inherited");
#else
   (void) setenv( "MULLE_SPAWN_MARKER", "inherited", 1);
#endif
}


/*
 * A capture printed line by line, so that a multi line answer reads as lines in
 * the record instead of as one string with newlines in the middle of it.
 */
static void   smoke_show_lines( const char *label, const char *text)
{
   const char   *nl;
   const char   *p;

   for( p = text; *p; p = nl + 1)
   {
      nl = strchr( p, '\n');
      if( ! nl)
      {
         printf( "     %s: %s\n", label, p);
         break;
      }
      printf( "     %s: %.*s\n", label, (int) (nl - p), p);
   }
}


/*
 * Runs the child with `options` and leaves what it printed in `buffer`.
 * Returns the number of bytes captured, or -1 if it could not be started.
 * The caller's options are borrowed; the handle is opened, drained and closed
 * here, which is all these cases need of one.
 */
static int   smoke_capture( char * const *argv,
                            struct mulle_spawn_options const *options,
                            char *buffer,
                            size_t capacity)
{
   struct mulle_spawn_handle   h;
   int                         n;

   if( mulle_spawn_open_with_options( &h, argv, options))
      return( -1);

   if( h.out < 0)
   {
      (void) mulle_spawn_close( &h, NULL);
      return( -1);
   }

   n = smoke_read_up_to( h.out, buffer, capacity - 1);

   (void) mulle_spawn_wait( &h, 5.0, NULL);
   (void) mulle_spawn_close( &h, NULL);
   return( n);
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

   if( argc >= 3 && ! strcmp( argv[ 1], "--environment"))
   {
      int   i;

      /* the names are the question; the answers are what it was launched with */
      for( i = 2; i < argc; i++)
         print_variable( argv[ i]);
      return( 0);
   }

   if( ! strcmp( argv[ 1], "--cwd"))
   {
      char   buffer[ 1024];

      if( ! smoke_getcwd( buffer, sizeof( buffer)))
         return( 1);

      /* the tail, not the path: a path is a machine's layout, not a fact */
      printf( "cwd=%s\n", smoke_basename( buffer));
      return( 0);
   }

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


/*
 * `<the directory this program is in>/<name>`, so a case can make something of
 * its own without knowing where it was put -- which is what keeps the same
 * assertion true under `mulle-sde test`, under a hand build in /tmp, and on a
 * machine that lays the tree out differently. Note the small `smoke_basename`
 * above: what goes into the record is the tail of the path, never the path.
 *
 * Returns 0, or -1 if it does not fit or this program has no known location.
 */
static int   smoke_beside_self( char *dst, size_t capacity, const char *name)
{
   size_t   length;
   size_t   wanted;

   if( ! self_found)
      return( -1);

   length = strlen( self_path);
   while( length && self_path[ length - 1] != '/' && self_path[ length - 1] != '\\')
      length--;

   wanted = length + strlen( name) + 1;
   if( wanted > capacity)
      return( -1);

   memcpy( dst, self_path, length);
   memcpy( dst + length, name, strlen( name) + 1);
   return( 0);
}


/*
 * The directory a child ran in, given back.
 *
 * On Windows the removal is not immediate: the platform keeps answering EACCES
 * for a short while after the child has been waited for and its process handle
 * closed, and releases the directory on its own within a tenth of a second or
 * so. Measured there (under wine, which is the only Windows this has run on),
 * the third attempt at 25 ms apart succeeds. Nothing in mulle-spawn is failing
 * here -- every assertion of the case has run by the time this is called -- so
 * a bounded retry is the honest way to give the directory back, rather than a
 * case that leaves litter behind on one platform.
 *
 * An errno that will not change for the better is not retried, and a directory
 * that could not be removed is reported on stderr, which is not part of the
 * record.
 */
static void   smoke_rmdir_when_released( const char *path)
{
   int   i;

   for( i = 0; i < 40; i++)
   {
      errno = 0;
      if( ! smoke_rmdir( path))
         return;
      if( errno != EACCES && errno != EBUSY)
         break;
      mulle_relativetime_sleep( 0.025);
   }

   fprintf( stderr, "smoke: could not remove <%s>, errno %d -- a leftover\n",
            path, errno);
}


/*
 * The directory a child runs in, which is not a nicety: a launch that names a
 * directory that is not there used to run the child wherever the parent was,
 * and that is indistinguishable from a child that worked.
 *
 * The directory is made here, beside this program, and given back however the
 * case ends.
 */
static void   test_directory( void)
{
   struct mulle_spawn_handle   h;
   struct mulle_spawn_options  options;
   char                        buffer[ 256];
   char                        *argv[ 3];
   char                        work[ 4096];
   int                         n;

   argv[ 0] = self_path;
   argv[ 1] = "--cwd";
   argv[ 2] = NULL;

   mulle_spawn_options_init( &options);
   options.out_mode = MULLE_SPAWN_PIPE;

   if( smoke_beside_self( work, sizeof( work), "spawn-cwd"))
   {
      check( "directory: beside itself is a place it knows", 0);
      return;
   }

   /* a leftover from a run that died is not a reason to skip the case */
   if( smoke_mkdir( work) && errno != EEXIST)
   {
      check( "directory: made somewhere to run", 0);
      return;
   }

   options.directory = work;

   memset( buffer, 0, sizeof( buffer));
   n = smoke_capture( argv, &options, buffer, sizeof( buffer));
   check( "directory: open returns 0", n >= 0);
   check( "directory: it ran there", ! strcmp( buffer, "cwd=spawn-cwd\n"));
   smoke_show_lines( "directory", buffer);

   /* and one that is not there fails the launch, rather than being ignored */
   options.directory = "/nonexistent/mulle-spawn-no-such-directory";

   errno = 0;
   check( "directory: one that is absent is refused",
          mulle_spawn_open_with_options( &h, argv, &options) == -1);
#ifndef _WIN32
   check( "directory: and errno says ENOENT", errno == ENOENT);
#endif

   smoke_rmdir_when_released( work);
}


/*
 * What the child is given to start with, which `mulle_spawn_open` cannot say
 * anything about -- a child inherits or it inherits.
 *
 * The same child is launched three times and asked the same two questions, and
 * the answers are expected to move:
 *
 *   inherited   NULL          the marker is there, the other name is not
 *   replaced    { TEST }      the marker is gone, the other name is there
 *   emptied     { NULL }      neither, which is not the same as "no opinion"
 *
 * The child is named by its absolute path throughout, so that none of this
 * depends on `PATH` surviving into the environment it is given.
 *
 * What a *given* environment contains is judged on POSIX only. On Windows the
 * only platform this can be run on is wine, and wine ignores CreateProcessW's
 * `lpEnvironment` outright: a child launched with a block of any kind sees an
 * empty environment -- Windows' own GetEnvironmentStringsW() handed straight
 * back included, which is what makes it the emulator rather than the block.
 * `probe/env-block-probe.c` is that measurement. So on Windows the record says
 * what the platform can honestly show: an environment that is handed over ends
 * the inheritance, and the parent's marker goes with it.
 */
static void   test_environment( void)
{
   struct mulle_spawn_options  options;
   char                        buffer[ 256];
   char                        *argv[ 5];
   char                        *emptied[ 1];
   char                        *replacement[ 2];
   int                         n;

   set_marker();

   argv[ 0] = self_path;
   argv[ 1] = "--environment";
   argv[ 2] = "MULLE_SPAWN_MARKER";
   argv[ 3] = "MULLE_SPAWN_TEST";
   argv[ 4] = NULL;

   replacement[ 0] = "MULLE_SPAWN_TEST=replaced";
   replacement[ 1] = NULL;

   emptied[ 0] = NULL;

   mulle_spawn_options_init( &options);
   options.out_mode = MULLE_SPAWN_PIPE;

   /* NULL: the parent's environment, marker and all */
   memset( buffer, 0, sizeof( buffer));
   n = smoke_capture( argv, &options, buffer, sizeof( buffer));
   check( "environment: inherited, open returns 0", n >= 0);
   check( "environment: inherited",
          ! strcmp( buffer, "MULLE_SPAWN_MARKER=inherited\n"
                            "MULLE_SPAWN_TEST=<unset>\n"));
   smoke_show_lines( "environment: inherited", buffer);

   /* a given one replaces rather than adds to the parent's */
   options.env = replacement;

   memset( buffer, 0, sizeof( buffer));
   n = smoke_capture( argv, &options, buffer, sizeof( buffer));
   check( "environment: replaced, open returns 0", n >= 0);
#ifndef _WIN32
   check( "environment: replaced",
          ! strcmp( buffer, "MULLE_SPAWN_MARKER=<unset>\n"
                            "MULLE_SPAWN_TEST=replaced\n"));
   smoke_show_lines( "environment: replaced", buffer);
#else
   check( "environment: replaced ends the inheritance",
          strstr( buffer, "MULLE_SPAWN_MARKER=<unset>") != NULL);
   printf( "     environment: replaced: the contents are judged on POSIX only\n");
#endif

   /* an empty environment is empty, and not the parent's */
   options.env = emptied;

   memset( buffer, 0, sizeof( buffer));
   n = smoke_capture( argv, &options, buffer, sizeof( buffer));
   check( "environment: emptied, open returns 0", n >= 0);
#ifndef _WIN32
   check( "environment: emptied",
          ! strcmp( buffer, "MULLE_SPAWN_MARKER=<unset>\n"
                            "MULLE_SPAWN_TEST=<unset>\n"));
   smoke_show_lines( "environment: emptied", buffer);
#else
   check( "environment: emptied ends the inheritance",
          strstr( buffer, "MULLE_SPAWN_MARKER=<unset>") != NULL);
   printf( "     environment: emptied: an empty block, left alone\n");
#endif
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
   test_environment();
   test_directory();

   printf( "\n%s (%d failures)\n", failures ? "FAILED" : "all mulle-spawn tests passed",
           failures);
   return( failures ? 1 : 0);
}
