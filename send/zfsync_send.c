/*
This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover
Twitter : @cedricmaunoury
Linkedin : cedric-maunoury
I'm currently looking for a remote job (from the netherlands)

How is this working ?!?
Have a look at zfshad.c header to get more informations
*/

#include <stdio.h> 
#include <stdlib.h> 
#include <unistd.h> 
#include <pthread.h> 
#include <semaphore.h>
#include <netdb.h> 
#include <netinet/in.h> 
#include <sys/socket.h> 
#include <sys/types.h> 
#include <sys/time.h>

#include <libzfs.h>
#include <libnvpair.h>
#include <libzfs_impl.h>

#define BUFFERSIZE 256
#define IPSIZE 40

// Let us create a global variable to change it in threads 
zfs_handle_t Zhp; //The Zfs Handle that will be used during the main loop, and then given to the workers to send the diff
sem_t MasterSem,WorkerSem; //Semaphores to coordinate the main loop and the workers (to avoid conflict when the Zfs Handle is given to the workers)
FILE LogFile; //Easy :)
static pthread_mutex_t LogMutex=PTHREAD_MUTEX_INITIALIZER; //Mutex to ensure that not everyone are writing on the log at the same time
struct timeval currentMicroSecond; //To write the current Time in the log file
time_t currentDateTime; //To write the current Date in the log file
char LocalIP[IPSIZE]; //IP to use to connect to RemoteIP
char RemoteIP[IPSIZE]; //IP used by zfshad (on the target)
int RemotePort; //Network port used by zfshad (on the target)
char LogFmt[BUFFERSIZE];
boolean_t LoopIsOver; //To let the workets know when everything is transferred

//Log function (maybe I log too much and the mutex may make it slower than expected, so we should use LogItMain and LogItThread as it's done on zfshad)
static void
LogIt(pthread_t tid, char const * __restrict fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  pthread_mutex_lock(&LogMutex);
  gettimeofday(&currentMicroSecond, NULL);
  time(&currentDateTime);
  struct tm *local = localtime(&currentDateTime);
  sprintf(&LogFmt, "%02d/%02d/%d %02d:%02d:%02d:%06d | th:%d | ", local->tm_mday, local->tm_mon + 1, local->tm_year + 1900, local->tm_hour, local->tm_min, local->tm_sec, currentMicroSecond.tv_usec, tid);
  vfprintf(&LogFile, strcat(strcat(LogFmt,fmt),"\n"), ap);
  pthread_mutex_unlock(&LogMutex);
  va_end(ap);
}

//To concatenate snapshot names available for a given dataset (@snap1@snap2@snap3...)
static int
concat_snapname(zfs_handle_t *zhp, void *arg)
{
  //LogIt(pthread_self(), "concat_snapname : %s", zhp->zfs_name);
  char *snaplist = arg;
  char *snapname = strrchr(zhp->zfs_name, '@');
  //printf("snapname : %s\n", snapname);
  //printf("snaplist before : %s\n", snaplist);
  if(snaplist==NULL) {
    //printf("snaplist is empty\n");
    strcpy(snaplist, snapname);
  } else {
    //strcpy(snaplist + strlen(snaplist), snapname);
    strcat(snaplist, snapname);
  }
  //printf("snaplist after : %s\n", snaplist); 
  return (0);
}

//To extract snapshot name of a dataset (@xyz)
static void
get_snapname(zfs_handle_t *zhp, void *arg)
{
  char *snapname = arg;
  strcpy(snapname, strrchr(zhp->zfs_name, '@'));
}

//To copy the Zfs Handle in the main loop to the global context
static int
send_zfsdiff(zfs_handle_t *zhp, void *arg)
{
  //printf("new dsname\n");
  //char *fd = arg;
  LogIt(pthread_self(), "(%s) Waiting for a sleeping thread", zhp->zfs_name);
  memcpy(&Zhp, zhp, sizeof(zfs_handle_t));
  sem_post(&WorkerSem);
  sem_wait(&MasterSem);
  return (0);
}

// The function to be executed by all threads 
// It waits for dataset to send, then open a connection, communicate with the receiver (see zfshad.c header for more details) and keeo the same connection until the end of the loop.
void *Worker(void *arg) 
{ 
    // Store the value argument passed to this thread 
    int fd=-1;
    libzfs_handle_t *g_zfs;
    char readbuf[BUFFERSIZE],writebuf[BUFFERSIZE],lastsnap[BUFFERSIZE];
    char *ds, *ptr;
    zfs_handle_t zhp;
    ssize_t readsize;
    struct sockaddr_in raddr, laddr;
    sendflags_t send_flags;

    if ((g_zfs = libzfs_init()) == NULL) {
      LogIt(pthread_self(), "Unable to init libzfs");
      exit(1);
    }
    libzfs_print_on_error(g_zfs, B_TRUE);

    raddr.sin_family = AF_INET;
    raddr.sin_addr.s_addr = inet_addr(RemoteIP);
    raddr.sin_port = htons(RemotePort);
    laddr.sin_family = AF_INET;
    laddr.sin_port = 0;
    laddr.sin_addr.s_addr = inet_addr(LocalIP);
    LogIt(pthread_self(), "Starting Worker to send ZFS stream to %s:%d (from %s)", RemoteIP, RemotePort, LocalIP);

    while (1) {
      LogIt(pthread_self(), "SEMAPHORE : Waiting...");
      bzero(readbuf,BUFFERSIZE);
      bzero(writebuf,BUFFERSIZE);
      bzero(&send_flags, sizeof(sendflags_t));
      send_flags.doall=B_TRUE;
      send_flags.props=B_TRUE;
      //send_flags.replicate=B_TRUE;
      send_flags.compress=B_TRUE;
      send_flags.verbose=B_TRUE;
      bzero(&zhp,sizeof(zfs_handle_t));
      sem_wait(&WorkerSem);
      if(LoopIsOver==B_TRUE) { 
        if(fd!=-1) {
          readsize=read(fd, readbuf, BUFFERSIZE);
          strcpy(writebuf, "END:sync");
          write(fd, writebuf, BUFFERSIZE); 
        }
        LogIt(pthread_self(), "Ending myself...");
        sem_post(&MasterSem);
        pthread_exit(NULL);
      }
      //we put in "ds" only the last part of the dataset name (zroot/test/local/dsname => dsname)
      //ds=strrchr(Zhp.zfs_name, '/');
      //LogIt(pthread_self(), "(%s) Working on %s", ds, Zhp.zfs_name);
      memcpy(&zhp, &Zhp,sizeof(zfs_handle_t));
      ds=strrchr(zhp.zfs_name, '/');
      LogIt(pthread_self(), "(%s) Zhp object copied", ds, zhp.zfs_name);
      sem_post(&MasterSem);
      //Genearating the snapshot names list : "@snap1@snap2..."
      (void) zfs_iter_snapshots_sorted(&zhp, concat_snapname, &writebuf, 0, 0);
      LogIt(pthread_self(), "(%s) zfs_iter_snapshots_sorted DONE", ds);
      if(fd!=-1) {
        readsize=read(fd, readbuf, BUFFERSIZE);
        LogIt(pthread_self(), "(%s) readbuf : %s", ds, readbuf);
        if(strcmp(readbuf,"0:OK")!=0) {
          LogIt(pthread_self(), "(%s) Close fd:%d", ds, fd);
          close(fd);
          fd=-1;
        }
        bzero(readbuf, BUFFERSIZE);
      } else {
      //if(fd==-1) {
        LogIt(pthread_self(), "(%s) Connecting to %s:%d (from %s)", ds, RemoteIP, RemotePort, LocalIP);
        fd=socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
          LogIt(pthread_self(), "(%s) Unable to connect to remote host [socket] (fd:%d)", ds, fd);
          goto GTEnd;
        }
        LogIt(pthread_self(), "(%s) fd:%d", ds, fd);
        if (bind(fd, &laddr, sizeof(struct sockaddr_in)) != 0) {
          LogIt(pthread_self(), "(%s) Unable to connect to remote host [bind] (fd:%d)", ds, fd);
          goto GTEnd;
        }
        if (connect(fd, &raddr, sizeof(struct sockaddr_in)) != 0) {
          LogIt(pthread_self(), "(%s) Unable to connect to remote host [connect] (fd:%d)", ds, fd);
          goto GTEnd;
        }
        LogIt(pthread_self(), "(%s) Connection OK (fd:%d)", ds, fd);
      //} else {
      //  LogIt(pthread_self(), "(%s) Already connected", ds, fd);
      }
      //Getting last snapshot
      ptr=strrchr(writebuf, '@');
      LogIt(pthread_self(), "(%s) strrchr DONE", ds);
      //We extract the last snapshot name to put it in lastsnap (will be used to check if lastsnap is already available on the remote)
      strcpy(lastsnap,ptr+1);
      LogIt(pthread_self(), "(%s) Last snapshot : %s", ds, lastsnap);
      strcat(writebuf, ":");
      strcat(writebuf, ds);
      strcat(writebuf, ":sync");
      LogIt(pthread_self(), "(%s) Sending on fd:%d : %s", ds, fd, writebuf);
      write(fd, writebuf, BUFFERSIZE);
      readsize=read(fd, readbuf, BUFFERSIZE);
      LogIt(pthread_self(), "(%s) readbuf : %s (size : %d)", ds, readbuf, readsize);
      //readbuf should normally be 0:(snapname|FULL|NEW) if everything is OK, 1:xyz if there's something wrong
      //0:snapname means send me zfs stream from snapname (that we both have)
      //0:FULL means we remote and local have no snapshot in common, so a full is required
      //O:NEW means this dataset does not exist on remote side
      ptr=strrchr(readbuf, ':');
      if(ptr==NULL) {
        LogIt(pthread_self(), "(%s) ERROR : Unable to parse readbuf", ds);
        goto GTEnd;
      }
      bzero(ptr,1);
      ptr=ptr+1;
      if(strcmp(readbuf,"1")==0) {
        LogIt(pthread_self(), "(%s) ERROR sent by remote", ds);
        goto GTEnd;
      }
      if(strcmp(ptr, lastsnap)==0) {
        //If lastsnap is already available on the remote side... nothing to do
        LogIt(pthread_self(), "(%s) Nothing to send", ds);
      } else if((strcmp(ptr, "FULL")==0)||(strcmp(ptr, "NEW")==0)) {
        //
        LogIt(pthread_self(), "(%s) Sending full ZFS stream (->%s) on fd:%d", ds, lastsnap, fd);
        if (zfs_send(&zhp, NULL, lastsnap, &send_flags, fd, NULL, 0, NULL)==0) {
          LogIt(pthread_self(), "(%s) ZFS stream succesfully sent", ds); 
        } else {
          LogIt(pthread_self(), "(%s) Error sending ZFS stream", ds);
        }
      } else {
        LogIt(pthread_self(), "(%s) Sending incremental ZFS stream (%s->%s) on fd:%d", ds, ptr, lastsnap, fd);
        if (zfs_send(&zhp, ptr, lastsnap, &send_flags, fd, NULL, 0, NULL)==0) {
          LogIt(pthread_self(), "(%s) ZFS stream succesfully sent", ds);
        } else {
          LogIt(pthread_self(), "(%s) Error sending ZFS stream", ds);
        }
      }
GTEnd:
      LogIt(pthread_self(), "(%s) End");
    }
    libzfs_fini(g_zfs);
} 

int
main(int argc, char *argv[]) 
{ 
    LogFile=*stdout;
    LogIt(pthread_self(), "Starting MAIN...");
    zfs_handle_t *zhp_main;
    int i; 
    char c;
    RemotePort=40;
    LoopIsOver=B_FALSE;
    strcpy(&LocalIP,"127.0.0.1");
    int maxthreads=4;
    char rdataset[BUFFERSIZE];
    boolean_t verbose = B_FALSE;

    while ((c = getopt(argc, argv, ":vp:i:b:t:")) != -1) {
        switch (c) {
        case 'v':
            if (verbose)
              verbose = B_TRUE;
            break;
        case 'p':
            RemotePort=atoi(optarg);
            break;
	case 't':
            maxthreads=atoi(optarg);
            break;
        case 'i':
            bzero(LocalIP,sizeof(char[40]));
            strcpy(LocalIP,optarg);
            break;
        case ':':
            (void) fprintf(stderr,
                "missing argument for '%c' option\n", optopt);
            //usage();
            break;
        case '?':
            (void) fprintf(stderr, "invalid option '%c'\n", optopt);
            //usage();
            break;
        }
    }

    if (optind!=argc-2) {
      LogIt(pthread_self(), "Wrong number of parameter (Dataset,RemoteIP)");
      exit(1);
    }
    
    strcpy(rdataset,argv[optind]);
    strcat(rdataset, "/local");
    strcpy(&RemoteIP,argv[optind+1]);
    
    pthread_t tid;

    // Let us create threads
    sem_init(&WorkerSem, 0, 0);
    sem_init(&MasterSem, 0, 0);
    for (i = 0; i < maxthreads; i++) {
        LogIt(pthread_self(), "pthread_create");
        pthread_create(&tid, NULL, Worker, (void*)NULL);
    }

    libzfs_handle_t *g_zfs_main;
    if ((g_zfs_main = libzfs_init()) == NULL) {
      LogIt(pthread_self(), "libzfs_init failure");
      exit(1);
    }
    libzfs_print_on_error(g_zfs_main, B_TRUE);

    zhp_main = zfs_open(g_zfs_main, rdataset, ZFS_TYPE_FILESYSTEM);
    if(zhp_main==NULL) {
      LogIt(pthread_self(), "Unable to open dataset");
      exit(1);
    }

    if(zfs_iter_filesystems(zhp_main, send_zfsdiff, NULL)==0) {
      LogIt(pthread_self(), "zfs_iter_filesystems : OK");
    } else {
      LogIt(pthread_self(), "zfs_iter_filesystems : ERROR");
    }

    LogIt(pthread_self(), "End of zfs_iter_filesystems");
    LoopIsOver=B_TRUE;

    for (i = 0; i < maxthreads; i++) {
      LogIt(pthread_self(), "Waiting for the end of a thread (%d)", i);
      sem_post(&WorkerSem);
      sem_wait(&MasterSem);
    }

    LogIt(pthread_self(), "Closing zhp_main");
    zfs_close(zhp_main);
  
    pthread_exit(NULL); 
    libzfs_fini(g_zfs_main);
    return 0; 
} 

