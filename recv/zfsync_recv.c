/*
This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover
Twitter : @cedricmaunoury
Linkedin : cedric-maunoury
I'm currently looking for a remote job (from the netherlands)

How is this working ?!?
ZfsHAd is in charge of receiving updates from ZfsHA (launched by the cron job zfshasnap.sh)

1/zfshasnap.sh creates snapshot on the Dataset to be synced and its children
2/It launches ZfsHA to send Dataset child snaps to ZfsHAd in a multithreaded way
3/Each thread is doing the following job :
    // It sends the snapshots name available for the current zhp to the ZfsHAd
    // -> "snap0@snap1@snap2:CHILDRENNAME:sync"
    // The receiver (ZfsHAd) replies with the last snapshot in common (or NEW or FULL if nothing matches)
    // <- "0:(snapX|NEW|FULL)"
    // Then a zfs_send is launched using the receiver reply to set the "from" in zfs_send
    // -> 0000110000111101011000... :)
    // If there's a new child dataset to handle, it uses the same connection to do the same work
    // ...
    // ...
    // When there is nothing more to do, it sends the following message to close the connection
    // -> "END:sync"

How to make it work :
- on the sender host, the dataset to be synced is $Dataset. What will be synced is the children of $Dataset/local => Put a cron to launch zfshasnap.sh regularly
- on the receiver host, the target dataset ($DatasetR) can have another name. What will be synced is the children of $Dataset/remote => Launch zfshad (to avoid issues, it can be good to set readonly=on on remote dataset)
Why local and remote subdataset ? The goal was to ensure no data loss in case of a "Split Brain" in a cluster architecture.

What is not handle for the moment by this code : 
- If a child dataset is destroyed on the source, it is not on the remote side
- No SSL... everything is clear on the network (could be very interesting as only one connection is opened by thread)
- Only a depth of 1 is handled. If you create a child dataset in a child dataset, it won't break anything, but it won't be synced.

Possible improvments :
- if we can detect that there's no change on the datas between two child snapshots, it could be interesting not to send the diff
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

#include <signal.h>
#include <libzfs.h>
#include <libnvpair.h>
#include <libzfs_impl.h>

//DEBUG
#include <sys/resource.h>

#define BUFFERSIZE 256 
#define IPSIZE 40
// Because of Memory Leaks in LibZFS
//#define MAXCONNPERTHREAD 1000

// Let us create a global variable to change it in threads 
//libzfs_handle_t *g_zfs;
char RootDataset[BUFFERSIZE];
int ConnFd; 
sem_t MasterSem,WorkerSem;
FILE* LogFile;
char LogName[BUFFERSIZE];
char FlagDirName[BUFFERSIZE];
static pthread_mutex_t LogMutex=PTHREAD_MUTEX_INITIALIZER;
struct timeval currentMicroSecond;
time_t currentDateTime;
char LocalIP[IPSIZE];
int LocalPort;
char LogFmt[BUFFERSIZE];
int ToReload[32];
int NbThread;

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

//To handle signal and then be able to manage logs correctly
static void
sig_handler(int signo)
{
  int i;
  LogItMain("Signal received (SIG:%d)\n", signo);
  for (i = 0; i < NbThread; i++) {
    ToReload[i]=signo;
  }
}

typedef struct zi_delete_absent_snap_cbdata {
  FILE tLogFile;
  struct timeval tcMS;
  time_t tcDT;
  int *fd;
  char *ds;
  char *cmd;
  char *list;
} zi_delete_absent_snap_cbdata_t;

static int
zi_delete_absent_snap(zfs_handle_t *zhp, void *arg)
{
  zi_delete_absent_snap_cbdata_t *cbd = arg;
  char *dsname = strrchr(zhp->zfs_name, '/');
  char *snapname = strrchr(dsname, '@');
  if(strstr(cbd->list, snapname)==NULL) {
    LogItThread(&cbd->tLogFile, cbd->tcMS, cbd->tcDT, cbd->fd, cbd->ds, cbd->cmd, "Deleting %s\n", dsname);
    if(zfs_destroy(zhp, B_FALSE)!=0) {
      LogItThread(&cbd->tLogFile, cbd->tcMS, cbd->tcDT, cbd->fd, cbd->ds, cbd->cmd, "ERROR deleting %s\n", dsname);
    }
  }
  zfs_close(zhp);
  return (0);
}

typedef struct zi_delete_snap_cbdata {
  FILE tLogFile;
  struct timeval tcMS;
  time_t tcDT;
  int *fd;
  char *ds;
  char *cmd;
} zi_delete_snap_cbdata_t;

static int
zi_delete_snap(zfs_handle_t *zhp, void *arg)
{
  zi_delete_snap_cbdata_t *cbd = arg;
  char *dsname = strrchr(zhp->zfs_name, '/');
  LogItThread(&cbd->tLogFile, cbd->tcMS, cbd->tcDT, cbd->fd, cbd->ds, cbd->cmd, "Deleting %s\n", dsname);
  if(zfs_destroy(zhp, B_FALSE)!=0) {
    LogItThread(&cbd->tLogFile, cbd->tcMS, cbd->tcDT, cbd->fd, cbd->ds, cbd->cmd, "ERROR deleting %s\n", dsname);
  }
  zfs_close(zhp);
  return (0);
}

// The function to be executed by all threads, cf zfsha.c for more details
void *Worker(void *arg) 
{ 
    int id = arg;
    //File descriptor of the incoming connection
    int fd=-1;
    //File descriptor to use to connect to another server
    int fd_ext;
    //LibZfs initialization
    libzfs_handle_t *g_zfs;
    //Buffer declaration
    char readbuf[BUFFERSIZE],writebuf[BUFFERSIZE], connbuf[BUFFERSIZE];
    //Pointer to get cmd,ds,lastsnap from connbuf
    char *cmd, *ds, *lastsnap,*options;
    //Pointer to be used in loops
    char *ptr;
    //Copy of RootDataset to store Dataset Full Name
    char rdataset[BUFFERSIZE];
    //ZFS Handles
    zfs_handle_t *zhp, *zhpr;
    //ZFS Flags
    recvflags_t recv_flags;
    renameflags_t rename_flags;
    sendflags_t send_flags;
    //Nv List
    nvlist_t propval, snaps;
    nvlist_t *props;
    //Network stuff
    struct sockaddr_in raddr, laddr;
    //Log
    FILE *tLogFile;
    char tLogName[BUFFERSIZE];
    struct timeval tcMS;
    time_t tcDT;
    struct tm *local;
    //One more :)
    ssize_t readsize;
    struct rusage memory;
    int nbconn=0;
    pthread_t tid;
    //var to log into zfs_iter functions
    zi_delete_absent_snap_cbdata_t zi_das_cbd;
    zi_das_cbd.tcMS=tcMS;
    zi_das_cbd.tcDT=&tcDT;
    zi_delete_snap_cbdata_t zi_ds_cbd;
    zi_ds_cbd.tcMS=tcMS;
    zi_ds_cbd.tcDT=&tcDT;
    //error flag
    char eflagname[BUFFERSIZE];

    // assign IP, PORT
    raddr.sin_family = AF_INET;
    laddr.sin_family = AF_INET;
    laddr.sin_port = 0;
    laddr.sin_addr.s_addr = inet_addr(LocalIP);

    if ((g_zfs = libzfs_init()) == NULL) {
      LogItMain("Unable to init libzfs\n");
      exit(1);
    }

    strcpy(readbuf,LogName);
    ptr=strrchr(readbuf,'.');
    bzero(ptr,1);
    sprintf(tLogName, "%s/th%d.%s", readbuf, pthread_self(), ptr+1);
    
    tLogFile=fopen(tLogName, "a");
    if(tLogFile==NULL) {
      LogItMain("Unable to fopen %s\n", tLogName);
      exit(1);
    }
    zi_das_cbd.tLogFile=*tLogFile;
    zi_ds_cbd.tLogFile=*tLogFile;

    LogItMain("Starting Worker (log:%s)\n", tLogName);
    LogItThread(tLogFile, tcMS, tcDT, 0, "X", "X", "Starting...(id=%d)\n",id);

    while (1) {
      nbconn++;
      //Emptying parameters...
      bzero(&recv_flags, sizeof(recvflags_t));
      recv_flags.force=B_TRUE;
      //recv_flags.verbose=B_TRUE; 
      recv_flags.isprefix=B_TRUE;
      bzero(&send_flags, sizeof(sendflags_t));
      send_flags.doall=B_TRUE;
      send_flags.props=B_TRUE;
      bzero(connbuf,BUFFERSIZE);
      bzero(readbuf,BUFFERSIZE);
      bzero(rdataset,BUFFERSIZE);
      strcpy(rdataset, &RootDataset);
      bzero(writebuf,BUFFERSIZE);
      if(fd==-1) {
        bzero(&fd,sizeof(int));
        if(ToReload[id]>0) { //A Mutex may be useful to avoid conflic on this data
          time(&tcDT);
          local = localtime(&tcDT);
          bzero(readbuf, BUFFERSIZE);
          switch(ToReload[id]) {
            case 30:
              sprintf(readbuf, "%s.%d%02d%02d-%02d%02d%02d", tLogName, local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
              LogItThread(tLogFile, tcMS, tcDT, 0, "X", "X", "SIGUSR1 received, renaming this log to %s\n", readbuf);
              fclose(tLogFile);
              rename(tLogName, readbuf);
              tLogFile=fopen(tLogName, "a");
              LogItThread(tLogFile, tcMS, tcDT, 0, "X", "X", "SIGUSR1 succesfully handled :-)\n");
              zi_das_cbd.tLogFile=*tLogFile;
              zi_ds_cbd.tLogFile=*tLogFile;
              ToReload[id]=0;
              break;
            case 31:
              LogItThread(tLogFile, tcMS, tcDT, 0, "X", "X", "SIGUSR2 received, respawning this thread\n");
              fclose(tLogFile);
              ToReload[id]=0;
              libzfs_fini(g_zfs);
              pthread_create(&tid, NULL, Worker, id);
              pthread_exit(NULL);
              break;
          }
        }
        sem_wait(&WorkerSem);
        memcpy(&fd,&ConnFd,sizeof(int));
        sem_post(&MasterSem);
      }
      readsize=read(fd, connbuf, BUFFERSIZE);
      LogItMain("fd:%d | connbuf='%s' (size:%d)\n", fd, connbuf, readsize);
      cmd=strrchr(connbuf, ':');
      if(cmd==NULL) {
        strcpy(writebuf, "1:cmd=NULL");
        goto GTLoopEnd;
      }
      bzero(cmd,1);
      cmd=cmd+1;
      ds=strrchr(connbuf, ':');
      if(ds==NULL) {
        ds=connbuf;
        options=NULL;
      }else{
        bzero(ds,1);
        ds=ds+1;
        options=connbuf;
      }
      getrusage(RUSAGE_SELF, &memory);
      LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "[Mem:%d] options='%s'\n", memory.ru_maxrss, options);
      //Preparing a flag in case os issue. Maybe useful for monitring concern
      strcpy(eflagname, FlagDirName);
      strcat(eflagname, ds);
      strcat(eflagname,".");
      strcat(eflagname,cmd);
      //LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Flag:%s\n", eflagname);
      //Checking the command part of the readbuf. Only "sync" is implemented for the moment, but "destroy" could be useful to destroy on the remote something that does not exist anymore
      if (strcmp(cmd,"sync")==0) {
        if(strcmp(ds,"END")==0) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Closing fd\n", writebuf);
          goto GTCloseFd;
        }
        strcat(rdataset, "/remote");
        strcpy(writebuf, "0:FULL");
        strcat(rdataset,ds);
        //if the dataset that is to be sent does not exist here, we ask for a full synchro (NEW)
        if(!zfs_dataset_exists(g_zfs, rdataset, ZFS_TYPE_FILESYSTEM)) {
          strcpy(writebuf, "0:NEW");
          goto GTSyncRecv; 
        } else {
          strcpy(readbuf, options);
          lastsnap = strrchr(options, '@')+1;
          ptr = strrchr(readbuf, '@');
          while(ptr!=NULL) {
            if(!zfs_dataset_exists(g_zfs, strcat(rdataset,ptr), ZFS_TYPE_SNAPSHOT)) {
              //If the snap does not exist, we try with the previous one in the list
              bzero(ptr,1);
              ptr = strrchr(readbuf, '@');
              bzero(rdataset, BUFFERSIZE);
              strcpy(rdataset, &RootDataset);
              strcat(rdataset, "/remote");
              strcat(rdataset,ds);
            } else {
              //If the snap exists, ask for an incremental send from this snap
              bzero(rdataset, BUFFERSIZE);
              strcpy(writebuf, "0:");
              strcat(writebuf, ptr+1);             
              break;
            }
          }
        }
        //if the lastsnap is already synced, nothing to do
        if(ptr!=NULL) {
          if(strcmp(lastsnap,ptr)==0) {
            goto GTSyncEnd;
          }
        }
GTSyncRecv:
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Sending on fd : %s\n", writebuf);
        //We send the request to the sender
        write(fd, writebuf, BUFFERSIZE);
        strcpy(rdataset, &RootDataset);
        strcat(rdataset, "/remote");
        //Force a refresh of libzfs_mnttab
        recv_flags.nomount=B_TRUE;
        recv_flags.istail=B_TRUE;
        libzfs_mnttab_fini(g_zfs);
        libzfs_mnttab_init(g_zfs);
        libzfs_mnttab_cache(g_zfs, B_TRUE);
        if(strcmp(writebuf, "0:NEW")==0) {
          //We prepare to receive the stream
          recv_flags.istail=B_TRUE;
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW) on fd => START\n", rdataset);
          if(zfs_receive(g_zfs, rdataset, NULL, &recv_flags, fd, NULL)==0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW) => OK\n");
            strcpy(writebuf, "0:OK");
          } else {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW) => ERROR\n");
            strcpy(writebuf, "1:zfs_receive(new)");
          }
        } else if (strcmp(writebuf, "0:FULL")==0) {

          strcat(rdataset,ds);
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Destroying %s\n", rdataset);
          zhp = zfs_open(g_zfs, rdataset, ZFS_TYPE_FILESYSTEM);
          if(zhp == NULL) {
            strcpy(writebuf, "1:zfs_open(full)");
            goto GTSyncEnd;
          }
          zi_ds_cbd.fd=fd;
          zi_ds_cbd.ds=ds;
          zi_ds_cbd.cmd=cmd;
          //We destroy all the existing snapshot for this dataset
          (void) zfs_iter_snapshots_sorted(zhp, zi_delete_snap, &zi_ds_cbd, 0, 0);
          if(zfs_destroy(zhp, B_FALSE)!=0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Unable to destroy %s\n", zhp->zfs_name);
            strcpy(writebuf, "1:zfs_destroy(full)");
            goto GTSyncEnd;
          }
          strcpy(rdataset, &RootDataset);
          strcat(rdataset, "/remote");
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) on fd => START\n", fd, ds, cmd, rdataset);
          //Then we prepare to receive the stream
          if(zfs_receive(g_zfs, rdataset, NULL, &recv_flags, fd, NULL)==0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) => OK\n");
            strcpy(writebuf, "0:OK");
          } else {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) => ERROR\n");
            strcpy(writebuf, "1:zfs_receive(full)");
          }
          zfs_close(zhp);
        }else{
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (INCR) on fd => START\n", rdataset);
          //Then we prepare to receive the stream
          if(zfs_receive(g_zfs, rdataset, NULL, &recv_flags, fd, NULL)==0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (INCR) => OK\n");
          } else {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (INCR) => ERROR\n");
            strcpy(writebuf, "1:zfs_receive(incr)");
            goto GTSyncEnd;
          }
          //We delete snapshots that are not available on the sender
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Cleaning useless snapshots\n");
          zhp = zfs_open(g_zfs, strcat(rdataset,ds), ZFS_TYPE_FILESYSTEM);
          if(zhp == NULL) {
            strcpy(writebuf, "1:zfs_open(incr)");
            goto GTSyncEnd;
          }
          strcpy(writebuf, "0:OK");
          zi_das_cbd.fd=fd;
          zi_das_cbd.ds=ds;
          zi_das_cbd.cmd=cmd;
          zi_das_cbd.list=options;
          (void) zfs_iter_snapshots_sorted(zhp, zi_delete_absent_snap, &zi_das_cbd, 0, 0);
          zfs_close(zhp);
        }
GTSyncEnd:
        libzfs_mnttab_cache(g_zfs, B_FALSE);
      } else {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Unknown cmd\n");
        //LogItMain("(%s:%s) Unknown cmd\n", ds, cmd); 
        strcpy(writebuf, "1:Unknown cmd ");
        strcat(writebuf, cmd);
      }
GTLoopEnd:
      if(cmd==NULL || ds==NULL) {
        LogItThread(tLogFile, tcMS, tcDT, fd, "X", "X", "Sending on fd : %s\n", writebuf);
        LogItMain("fd:%d | (UNKNOWN) Sending on fd : %s\n", fd, writebuf);
      } else {
        LogItMain("fd:%d | (%s:%s) Sending on fd : %s\n", fd, ds, cmd, writebuf);
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Sending on fd : %s\n", writebuf);
        if(strcmp(writebuf,"0:OK")==0) {
          remove(eflagname);
        } else {
          symlink("ERROR", eflagname); 
        }
      }
      write(fd, writebuf, BUFFERSIZE);
      if(strcmp(writebuf,"0:OK")==0 && strcmp(cmd,"sync")==0) {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Keeping this fd up\n");
      } else {
GTCloseFd:
        close(fd);
        fd=-1;
      }
    }
} 

int
main(int argc, char *argv[]) 
{ 
    //LogFile=stdout;
    strcpy(LogName,"/var/log/zfshad.log");
    strcpy(FlagDirName,"/var/run/zfshad");
    int i; 
    char c;
    LocalPort=30;
    strcpy(&LocalIP,"127.0.0.1");
    int backlog=50;
    NbThread=4;
    boolean_t verbose = B_FALSE;
    struct rusage memory;

    while ((c = getopt(argc, argv, ":vp:i:b:t:o:d:")) != -1) {
        switch (c) {
        case 'v':
            if (verbose)
              verbose = B_TRUE;
            break;
        case 'p':
            LocalPort=atoi(optarg);
            break;
        case 'b':
	    backlog=atoi(optarg);
            break;
	case 't':
            NbThread=atoi(optarg);
            break;
        case 'o':
            strcpy(LogName,optarg);
            break;
        case 'd':
            strcpy(FlagDirName,optarg);
            break;
        case 'i':
            bzero(&LocalIP,sizeof(char[40]));
            strcpy(&LocalIP,optarg);
            break;
        case ':':
            (void) fprintf(stderr,
                "missing argument for '%c' option\n", optopt);
            //usage();
            break;
        case '?':
            (void) fprintf(stderr, "invalid option '%c'\n", optopt);
            exit(1);
            break;
        }
    }

    LogFile=fopen(LogName, "a");
    if(LogFile==NULL) {
      (void) fprintf(stderr, "invalid option '%c'\n", optopt);
      exit(1);
    }
    LogItMain("Starting MAIN\n");

    if (signal(SIGUSR1, sig_handler) == SIG_ERR) {
      LogItMain("Cannot handle SIGUSR1 properly\n");
      exit(1);
    }
    if (signal(SIGUSR2, sig_handler) == SIG_ERR) {
      LogItMain("Cannot handle SIGUSR2 properly\n");
      exit(1);
    }

    strcat(FlagDirName, "/");
    if(mkdir(FlagDirName, 0700) && errno != EEXIST) {
      LogItMain("Directory to store error flags unavailable (%s)\n", FlagDirName);
      exit(1);
    }
    LogItMain("Directory to store error flags available (%s)\n", FlagDirName);

    if (optind==argc) {
      LogItMain("Target dataset has not been given...\n");
      exit(1);
    }

    strcpy(&RootDataset,argv[optind]);
    pthread_t tid;
    int sockfd,connfd;
    struct sockaddr_in servaddr, cli; 
    size_t len;
  
    // socket create and verification 
    //LogIt(pthread_self(), "Socket creation on %s:%d", LocalIP, LocalPort);
    LogItMain("Socket creation on %s:%d\n", LocalIP, LocalPort);
    sockfd = socket(AF_INET, SOCK_STREAM, 0); 
    if (sockfd == -1) { 
      LogItMain("Socket creation failed...\n");
      exit(1); 
    } 
    else
      LogItMain("Socket successfully created.\n");
    bzero(&servaddr, sizeof(servaddr)); 
    // assign IP, PORT 
    servaddr.sin_family = AF_INET; 
    servaddr.sin_addr.s_addr = inet_addr(LocalIP);
    servaddr.sin_port = htons(LocalPort); 
  
    // Binding newly created socket to given IP and verification 
    if ((bind(sockfd, &servaddr, sizeof(servaddr))) != 0) { 
      LogItMain("Socket bind failed...\n");
      exit(1); 
    } 
    else
      LogItMain("Socket successfully binded...\n"); 
  
    // Now server is ready to listen and verification 
    if ((listen(sockfd, backlog)) != 0) {
      LogItMain("Listen failed...\n"); 
      exit(1); 
    } 
    else
      LogItMain("Server listening...\n");
    len = sizeof(cli); 

    // Let us create threads
    sem_init(&WorkerSem, 0, 0);
    sem_init(&MasterSem, 0, 0);
    for (i = 0; i < NbThread; i++) {
        //LogIt(pthread_self(), "pthread_create");
        //pthread_create(&tid, NULL, Worker, (void*)NULL);
        ToReload[i]=0;
        //pthread_create(&tid, NULL, Worker, &ToReload+i);
        //tToReload=ToReload[i];
        pthread_create(&tid, NULL, Worker, i);
    }

    sleep(1);

    //LogItMain("Updating ToReloads to B_FALSE...\n");   
    //for (i = 0; i < NbThread; i++) {
    //    ToReload[i]=B_FALSE;
    //}

    while (1) { 
      //Accept the data packet from client and verification 
      ConnFd = accept(sockfd, &cli, &len); 
      if (ConnFd < 0) { 
        LogItMain("Unable to accept new connection\n");
        exit(0); 
      } 
      //LogIt(pthread_self(), "New connection (ConnFd:%d)", ConnFd);
      //getrusage(RUSAGE_SELF, &memory);
      //LogIt(pthread_self(), "[Mem:%d]New connection (ConnFd:%d)", memory.ru_maxrss , ConnFd);
      //LogIt(pthread_self(), "SEMAPHORE : Posting Worker (mem:%d)", memory.ru_maxrss);
      LogItMain("fd:%d | New connection\n", ConnFd);
      sem_post(&WorkerSem);
      //LogIt(pthread_self(), "SEMAPHORE : Waiting Master");
      sem_wait(&MasterSem);
    }
  
    pthread_exit(NULL); 
    //libzfs_fini(g_zfs);
    free(ConnFd);
    return 0; 
} 

