/*
This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover
Twitter : @cedricmaunoury
Linkedin : cedric-maunoury
I'm currently looking for a remote job (from the netherlands)

How is this working ?!? Please habe a look at README file
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
FILE* LogFile; //Easy :)
static pthread_mutex_t LogMutex=PTHREAD_MUTEX_INITIALIZER; //Mutex to ensure that not everyone are writing on the log at the same time
static pthread_mutex_t EndMutex=PTHREAD_MUTEX_INITIALIZER; //Mutex to ensure threads are ending one by one
struct timeval currentMicroSecond; //To write the current Time in the log file
time_t currentDateTime; //To write the current Date in the log file
char LocalIP[IPSIZE]; //IP to use to connect to RemoteIP
char RemoteIP[IPSIZE]; //IP used by zfshad (on the target)
int RemotePort; //Network port used by zfshad (on the target)
char LogFmt[BUFFERSIZE];
boolean_t LoopIsOver; //To let the workets know when everything is transferred
size_t DatasetLen;
char LogName[BUFFERSIZE];
boolean_t Verbose = B_FALSE;
int NbThreads = 4;

//LogItThread and LogItMain are here to log events. A mutex is used in LogItMain to avoid conflict.
static void
LogItMain(char const * __restrict fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&LogMutex);
  gettimeofday(&currentMicroSecond, NULL);
  time(&currentDateTime);
  struct tm *local = localtime(&currentDateTime);
  fprintf(LogFile,"%02d/%02d/%d %02d:%02d:%02d:%06ld | th:%d | ", local->tm_mday, local->tm_mon + 1, local->tm_year + 1900, local->tm_hour, local->tm_min, local->tm_sec, currentMicroSecond.tv_usec, pthread_self());
  vfprintf(LogFile,fmt,ap);
  va_end(ap);
  pthread_mutex_unlock(&LogMutex);
  fflush(LogFile);
}

static void
LogItThread(FILE *tLogFile, struct timeval tcurrentMicroSecond, time_t tcurrentDateTime, int *fd, char *ds, char *cmd, char const * __restrict fmt, ...)
{
  if(Verbose == B_TRUE) {
    va_list ap;
    va_start(ap, fmt);
    gettimeofday(&tcurrentMicroSecond, NULL);
    time(&tcurrentDateTime);
    struct tm *local = localtime(&tcurrentDateTime);
    fprintf(tLogFile,"%02d/%02d/%d %02d:%02d:%02d:%06ld | fd:%d | (%s:%s) ", local->tm_mday, local->tm_mon + 1, local->tm_year + 1900, local->tm_hour, local->tm_min, local->tm_sec, tcurrentMicroSecond.tv_usec, fd, ds, cmd);
    vfprintf(tLogFile,fmt,ap);
    va_end(ap);
    fflush(tLogFile);
  }
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
  LogItMain("(%s) Waiting for a sleeping thread\n", zhp->zfs_name);
  memcpy(&Zhp, zhp, sizeof(zfs_handle_t));
  sem_post(&WorkerSem);
  sem_wait(&MasterSem);
  zfs_iter_filesystems(zhp, send_zfsdiff, NULL);
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
    //Log
    FILE *tLogFile;
    char tLogName[BUFFERSIZE];
    struct timeval tcMS;
    time_t tcDT;
    struct tm *local;

    if ((g_zfs = libzfs_init()) == NULL) {
      LogItMain("Unable to init libzfs");
      exit(1);
    }
    libzfs_print_on_error(g_zfs, B_TRUE);

    raddr.sin_family = AF_INET;
    raddr.sin_addr.s_addr = inet_addr(RemoteIP);
    raddr.sin_port = htons(RemotePort);
    laddr.sin_family = AF_INET;
    laddr.sin_port = 0;
    laddr.sin_addr.s_addr = inet_addr(LocalIP);

    strcpy(readbuf,LogName);
    ptr=strrchr(readbuf,'.');
    bzero(ptr,1);
    sprintf(tLogName, "%s.th%d.%s", readbuf, pthread_self(), ptr+1);

    tLogFile=fopen(tLogName, "a");
    if(tLogFile==NULL) {
      LogItMain("Unable to fopen %s\n", tLogName);
      exit(1);
    }

    LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "Starting Worker to send ZFS stream to %s:%d (from %s)\n", RemoteIP, RemotePort, LocalIP);

    while (1) {
      LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "SEMAPHORE : Waiting...\n");
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
        LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "Locking EndMutex\n");
        pthread_mutex_lock(&EndMutex);
        LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "EndMutex locked\n");
        NbThreads--;
        if(fd!=-1) {
          readsize=read(fd, readbuf, BUFFERSIZE);
          LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "%d threads remaining\n", NbThreads);
          if(NbThreads==0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "I'm the last thread : Asking for a cleanbuffer\n");
            strcpy(writebuf, ":cleanbuffer");
            write(fd, writebuf, BUFFERSIZE);
            readsize=read(fd, readbuf, BUFFERSIZE);
            LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "readbuf : %s\n", readbuf);
          }
          strcpy(writebuf, ":close");
          write(fd, writebuf, BUFFERSIZE); 
        }
        LogItMain("Ending myself...\n");
        sem_post(&MasterSem);
        pthread_mutex_unlock(&EndMutex);
        LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "EndMutex unlocked\n");
        pthread_exit(NULL);
      }
      //we put in "ds" only the last part of the dataset name (zroot/test/local/dsname => dsname)
      //ds=strrchr(Zhp.zfs_name, '/');
      //LogIt(pthread_self(), "(%s) Working on %s", ds, Zhp.zfs_name);
      memcpy(&zhp, &Zhp,sizeof(zfs_handle_t));
      //ds=strrchr(zhp.zfs_name, '/');
      ds=zhp.zfs_name+DatasetLen;
      LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Zhp object copied (%s)\n", zhp.zfs_name);
      sem_post(&MasterSem);
      //Genearating the snapshot names list : "@snap1@snap2..."
      (void) zfs_iter_snapshots_sorted(&zhp, concat_snapname, &writebuf, 0, 0);
      LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "zfs_iter_snapshots_sorted DONE\n");
      if(fd!=-1) {
        readsize=read(fd, readbuf, BUFFERSIZE);
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "readbuf : %s\n", readbuf);
        if(strcmp(readbuf,"0:OK")!=0) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Closing fd\n");
          close(fd);
          fd=-1;
        }
        bzero(readbuf, BUFFERSIZE);
      } else {
      //if(fd==-1) {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Connecting to %s:%d (from %s)\n", RemoteIP, RemotePort, LocalIP);
        fd=socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Unable to connect to remote host [socket]\n");
          goto GTEnd;
        }
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "fd created\n");
        if (bind(fd, &laddr, sizeof(struct sockaddr_in)) != 0) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Unable to connect to remote host [bind]\n");
          goto GTEnd;
        }
        if (connect(fd, &raddr, sizeof(struct sockaddr_in)) != 0) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Unable to connect to remote host [connect]\n");
          goto GTEnd;
        }
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Connection OK\n");
      //} else {
      //  LogIt(pthread_self(), "(%s) Already connected", ds, fd);
      }
      //Getting last snapshot
      ptr=strrchr(writebuf, '@');
      if(ptr==NULL) {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "No snapshot available on this dataset\n");
        goto GTEnd;
      }
      //LogIt(pthread_self(), "(%s) strrchr DONE", ds);
      //We extract the last snapshot name to put it in lastsnap (will be used to check if lastsnap is already available on the remote)
      strcpy(lastsnap,ptr+1);
      //LogIt(pthread_self(), "(%s) Last snapshot : %s", ds, lastsnap);
      strcat(writebuf, ":");
      strcat(writebuf, ds);
      strcat(writebuf, ":sync");
      LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Sending : %s\n", writebuf);
      write(fd, writebuf, BUFFERSIZE);
      readsize=read(fd, readbuf, BUFFERSIZE);
      LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Received : %s (size:%d)\n", readbuf, readsize);
      //readbuf should normally be 0:(snapname|FULL|NEW) if everything is OK, 1:xyz if there's something wrong
      //0:snapname means send me zfs stream from snapname (that we both have)
      //0:FULL means we remote and local have no snapshot in common, so a full is required
      //O:NEW means this dataset does not exist on remote side
      ptr=strrchr(readbuf, ':');
      if(ptr==NULL) {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Unable to parse readbuf\n");
        goto GTEnd;
      }
      bzero(ptr,1);
      ptr=ptr+1;
      if(strcmp(readbuf,"1")==0) {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "ERROR sent by remote : %s\n", ptr);
        goto GTEnd;
      }
      if(strcmp(ptr, lastsnap)==0) {
        //If lastsnap is already available on the remote side... nothing to do
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Nothing to send\n");
      } else if((strcmp(ptr, "FULL")==0)||(strcmp(ptr, "NEW")==0)) {
        //
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Sending full ZFS stream (->%s)\n", lastsnap);
        if (zfs_send(&zhp, NULL, lastsnap, &send_flags, fd, NULL, 0, NULL)==0) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "ZFS stream succesfully sent\n");
        } else {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Error sending ZFS stream\n");
        }
      } else {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Sending incremental ZFS stream (%s->%s)\n", ptr, lastsnap);
        if (zfs_send(&zhp, ptr, lastsnap, &send_flags, fd, NULL, 0, NULL)==0) {
	  LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "ZFS stream succesfully sent\n");
        } else {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, "sync", "Error sending ZFS stream\n");
        }
      }
GTEnd:
      LogItMain("End\n");
    }
    libzfs_fini(g_zfs);
} 

int
main(int argc, char *argv[]) 
{ 
    //LogFile=*stdout;
    //LogIt(pthread_self(), "Starting MAIN...");
    zfs_handle_t *zhp_main;
    int i; 
    char c;
    RemotePort=40;
    LoopIsOver=B_FALSE;
    strcpy(&LocalIP,"127.0.0.1");
    char rdataset[BUFFERSIZE];

    while ((c = getopt(argc, argv, ":vp:t:i:o:")) != -1) {
        switch (c) {
        case 'v':
            Verbose = B_TRUE;
            break;
        case 'p':
            RemotePort=atoi(optarg);
            break;
	case 't':
            NbThreads=atoi(optarg);
            break;
        case 'i':
            bzero(LocalIP,sizeof(char[40]));
            strcpy(LocalIP,optarg);
            break;
        case 'o':
            strcpy(LogName,optarg);
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

    LogFile=fopen(LogName, "a");
    if(LogFile==NULL) {
      (void) fprintf(stderr, "invalid option '%c'\n", optopt);
      exit(1);
    }
    LogItMain("Starting MAIN\n");


    if (optind!=argc-2) {
      LogItMain("Wrong number of parameter (Dataset,RemoteIP)\n");
      exit(1);
    }
    
    strcpy(rdataset,argv[optind]);
    //strcat(rdataset, "/local");
    DatasetLen=strlen(rdataset);
    LogItMain("rdataset : %s (%d)\n", rdataset, DatasetLen);
    strcpy(&RemoteIP,argv[optind+1]);
    
    pthread_t tid;

    // Let us create threads
    sem_init(&WorkerSem, 0, 0);
    sem_init(&MasterSem, 0, 0);
    for (i = 0; i < NbThreads; i++) {
        LogItMain("pthread_create\n");
        pthread_create(&tid, NULL, Worker, (void*)NULL);
    }

    libzfs_handle_t *g_zfs_main;
    if ((g_zfs_main = libzfs_init()) == NULL) {
      LogItMain("libzfs_init failure");
      exit(1);
    }
    libzfs_print_on_error(g_zfs_main, B_TRUE);

    zhp_main = zfs_open(g_zfs_main, rdataset, ZFS_TYPE_FILESYSTEM);
    if(zhp_main==NULL) {
      LogItMain("Unable to open dataset\n");
      exit(1);
    }

    //if(zfs_iter_filesystems(zhp_main, send_zfsdiff, NULL)==0) {
    //  LogIt(pthread_self(), "zfs_iter_filesystems : OK");
    //} else {
    //  LogIt(pthread_self(), "zfs_iter_filesystems : ERROR");
    //}
    pthread_mutex_lock(&EndMutex);
    send_zfsdiff(zhp_main, NULL);

    LogItMain("End of zfs_iter_filesystems\n");
    LoopIsOver=B_TRUE;

    for (i = 0; i < NbThreads; i++) {
      sem_post(&WorkerSem);
      LogItMain("WorkerSem posted (%d)\n", i);
    }
    int LocalNbThreads=NbThreads;
    LogItMain("Unlocking EndMutex\n");
    pthread_mutex_unlock(&EndMutex);
    for (i = 0; i < LocalNbThreads; i++) {
      LogItMain("Waiting for the end of a thread (%d)\n", i);
      sem_wait(&MasterSem);
    }

    LogItMain("Closing zhp_main\n");
    zfs_close(zhp_main);
  
    pthread_exit(NULL); 
    libzfs_fini(g_zfs_main);
    return 0; 
} 

