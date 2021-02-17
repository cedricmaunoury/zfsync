/*
This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover
Twitter : @cedricmaunoury
Linkedin : cedric-maunoury
I'm currently looking for a remote job (from the netherlands)

How is this working ?!? Please have a look at README file
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
boolean_t Verbose = B_FALSE;
libzfs_handle_t* G_zfs;

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


int
bin2hex(char *bin, char *hex)
{
  size_t  i,len;

  len=strlen(bin);

  if (bin == NULL || len == 0)
    return 0;

  for (i=0; i<len; i++) {
    hex[i*2]   = "0123456789ABCDEF"[bin[i] >> 4];
    hex[i*2+1] = "0123456789ABCDEF"[bin[i] & 0x0F];
  }
  hex[len*2] = '\0';
  return len*2;
}

int hexchr2bin(const char hex, char *out)
{
  if (out == NULL)
    return 0;

  if (hex >= '0' && hex <= '9') {
    *out = hex - '0';
  } else if (hex >= 'A' && hex <= 'F') {
    *out = hex - 'A' + 10;
  } else if (hex >= 'a' && hex <= 'f') {
    *out = hex - 'a' + 10;
  } else {
    return 0;
  }

  return 1;
}


int
hex2bin(char *hex, char *bin)
{
  size_t len;
  char   b1;
  char   b2;
  size_t i;

  if (hex == NULL || *hex == '\0' || bin == NULL) {
    LogItMain("hex2bin:1\n");
    return 0;
  }

  len = strlen(hex);
  if (len % 2 != 0) {
    LogItMain("hex2bin:2\n");
    return 0;
  }
  len /= 2;

  memset(bin, 'A', len);
  for (i=0; i<len; i++) {
    if (!hexchr2bin(hex[i*2], &b1) || !hexchr2bin(hex[i*2+1], &b2)) {
      LogItMain("hex2bin:3\n");
      return 0;
    }
    bin[i] = (b1 << 4) | b2;
  }
  bin[len] = '\0';
  return len;
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
  //LogItThread(&cbd->tLogFile, cbd->tcMS, cbd->tcDT, cbd->fd, cbd->ds, cbd->cmd, "zfs_close to come in zi_delete_absent_snap (%s)\n", dsname);
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
  LogItThread(&cbd->tLogFile, cbd->tcMS, cbd->tcDT, cbd->fd, cbd->ds, cbd->cmd, "zfs_close to come in zi_delete_snap (%s)\n", dsname);
  zfs_close(zhp);
  return (0);
}

typedef struct zi_rename_cbdata {
  FILE tLogFile;
  struct timeval tcMS;
  time_t tcDT;
  int *fd;
  char *ds;
  char *cmd;
} zi_rename_cbdata_t;

static int
zi_rename_bufdataset(zfs_handle_t *zhp, char *rdataset)
{
  //LogItMain("Buffered dataset found : %s\n", zhp->zfs_name);
  renameflags_t flags;
  char* ptr;
  ptr = strrchr(zhp->zfs_name, '/')+1; //+1 to avoid the /
  LogItMain("Buffered dataset found : %s\n", ptr);
  strcpy(rdataset, &RootDataset);
  char *realdsname = rdataset+strlen(rdataset);
  if(hex2bin(ptr, realdsname)==0) {
    LogItMain("Error during hex2bin encoding\n");
    return 1;
  }
  LogItMain("hex2bin : %s\n", rdataset);
  if(zfs_dataset_exists(G_zfs, rdataset, ZFS_TYPE_FILESYSTEM)) {
    //The target dataset already exist, so we will just destroy the current zhp to avoid conflict
    LogItMain("Target dataset already exist : Destroying buffered dataset %s\n", ptr);
    (void) zfs_iter_snapshots(zhp, B_TRUE, zfs_destroy, B_FALSE, 0, 0);
    if(zfs_destroy(zhp, B_FALSE)!=0) {
      LogItMain("Error destroying buffered dataset %s\n", ptr);
      zfs_close(zhp);
      return 1;
    } else {
      LogItMain("Buffered dataset %s succesfully destroyed\n", ptr);
      zfs_close(zhp);
      return 0;
    }
  }
  char rdataset_parent[BUFFERSIZE];
  strcpy(rdataset_parent, rdataset);
  ptr=strrchr(rdataset_parent, '/');
  if(ptr==NULL) {
    LogItMain("Parent dataset not found\n");
    return 1;
  }
  bzero(ptr,1);
  if(!zfs_dataset_exists(G_zfs, rdataset_parent, ZFS_TYPE_FILESYSTEM)) {
    LogItMain("Parent dataset does not exist (%s). We have to rename buffered parent dataset first\n", rdataset_parent);
    char hexdsname_parent[BUFFERSIZE];
    char *realdsname_parent= rdataset_parent+strlen(RootDataset);
    if(bin2hex(realdsname_parent, hexdsname_parent)==0) {
      LogItMain("Error extracting hexadecimal version of parent dataset\n");
      return 1;
    }
    LogItMain("Buffered parent dataset : %s\n", hexdsname_parent);
    strcpy(rdataset_parent, &RootDataset);
    strcat(rdataset_parent,".zfsyncbuffer/");
    strcat(rdataset_parent,hexdsname_parent);
    zfs_handle_t *zhp_parent;
    zhp_parent = zfs_open(G_zfs, rdataset_parent, ZFS_TYPE_FILESYSTEM);
    if(zhp_parent == NULL) {
      LogItMain("Cannot open parent dataset (%s)\n", rdataset);
      return 1;
    }
    if(zi_rename_bufdataset(zhp_parent, rdataset_parent)==0) {
      LogItMain("Buffered parent dataset has been succesfully renamed\n");
    } else {
      LogItMain("Error renaming parent dataset\n");
      return 1;
    }
  } 
  ptr = strrchr(zhp->zfs_name, '/')+1;
  LogItMain("Renaming %s to %s\n", ptr, rdataset);
  #ifdef HAVE_ZOL
    if(zfs_rename(zhp, rdataset, flags)==0) {
      LogItMain("Rename OK\n");
      return 0;
    }
  #else
    if(zfs_rename(zhp, NULL, rdataset, flags)==0) {
      LogItMain("Rename OK\n");
      return 0;
    }
  #endif
  LogItMain("Rename ERROR\n");
  return 1;
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
    zi_rename_cbdata_t zi_r_cbd;
    zi_r_cbd.tcMS=tcMS;
    zi_r_cbd.tcDT=&tcDT;
    //https://github.com/openzfs/zfs/pull/11608
    int is_incremental;
    
    

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
    sprintf(tLogName, "%s.th%d.%s", readbuf, pthread_self(), ptr+1);
    
    tLogFile=fopen(tLogName, "a");
    if(tLogFile==NULL) {
      LogItMain("Unable to fopen %s\n", tLogName);
      exit(1);
    }
    zi_das_cbd.tLogFile=*tLogFile;
    zi_ds_cbd.tLogFile=*tLogFile;
    zi_r_cbd.tLogFile=*tLogFile;

    LogItMain("Starting Worker (log:%s)\n", tLogName);
    LogItThread(tLogFile, tcMS, tcDT, 0, "X", "X", "Starting...(id=%d)\n",id);

    while (1) {
      nbconn++;
      //Emptying parameters...
      bzero(&recv_flags, sizeof(recvflags_t));
      recv_flags.force=B_TRUE;
      //recv_flags.verbose=B_TRUE; 
      recv_flags.isprefix=B_TRUE;
      //recv_flags.force=B_TRUE;
      bzero(&send_flags, sizeof(sendflags_t));
      send_flags.doall=B_TRUE;
      send_flags.props=B_TRUE;
      bzero(connbuf,BUFFERSIZE);
      bzero(readbuf,BUFFERSIZE);
      bzero(rdataset,BUFFERSIZE);
      strcpy(rdataset, &RootDataset);
      bzero(writebuf,BUFFERSIZE);
      is_incremental=1;
      if(fd==-1) {
        bzero(&fd,sizeof(int));
        if(ToReload[id]>0) { //A Mutex may be useful to avoid conflict on this data
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
      //Checking the command part of the readbuf. "destroy" could be useful to destroy on the remote something that does not exist anymore
      if (strcmp(cmd,"close")==0) {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Closing fd\n");
        goto GTCloseFd;
      } else if (strcmp(cmd,"cleanbuffer")==0) {
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Cleaning buffer\n"); 
        strcpy(rdataset, &RootDataset);
        strcat(rdataset,".zfsyncbuffer");
        zhp = zfs_open(g_zfs, rdataset, ZFS_TYPE_FILESYSTEM);
        if(zhp == NULL) {
          strcpy(writebuf, "1:zfs_open(buffer)");
          goto GTSyncEnd;
        }
        //zi_r_cbd.fd=fd;
        //LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "ds=readbuf\n");
        //ds=readbuf;
        //LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zi_r_cbd.ds=ds\n");
        //zi_r_cbd.ds=ds;
        //LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zi_r_cbd.cmd=cmd\n");
        //zi_r_cbd.cmd=cmd;
        LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Starting the zfs_iter on %s\n", rdataset);
        if(zfs_iter_filesystems(zhp, zi_rename_bufdataset, rdataset)==0) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_iter succesfully ended\n");
        } else {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_iter went wrong\n");
        }
        strcpy(writebuf, "0:OK");
        goto GTLoopEnd;
      } else if (strcmp(cmd,"sync")==0) {
        //strcat(rdataset, "/remote");
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
              //strcat(rdataset, "/remote");
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
        //strcat(rdataset, "/remote");
        //Force a refresh of libzfs_mnttab
        recv_flags.nomount=B_TRUE;
        recv_flags.istail=B_TRUE;
        libzfs_mnttab_fini(g_zfs);
        libzfs_mnttab_init(g_zfs);
        libzfs_mnttab_cache(g_zfs, B_TRUE);
        strcat(rdataset,ds);
        if(strcmp(writebuf, "0:NEW")==0) {
          //Extracting the parent dataset name
          //ptr = strrchr(rdataset, '/');
          //bzero(ptr,1);
          recv_flags.isprefix=B_FALSE;
          recv_flags.istail=B_FALSE;
          //recv_flags.force=B_TRUE;
          //LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW) on fd => START (rdataset:%s)\n", rdataset);
          //We have to check if the parent dataset exists
          ptr = strrchr(rdataset, '/');
          bzero(ptr,1);
          if(zfs_dataset_exists(g_zfs, rdataset, ZFS_TYPE_FILESYSTEM)) {
            strcpy(rdataset, &RootDataset);
            strcat(rdataset,ds);
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW) on fd => START (rdataset:%s)\n", rdataset);
          } else {
            strcpy(rdataset, &RootDataset);
            strcat(rdataset,".zfsyncbuffer");
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Trying to create Buffer dataset : %s\n", rdataset);
            zfs_create(g_zfs, rdataset, ZFS_TYPE_FILESYSTEM, props);
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Creation of Buffer dataset ended\n");
            libzfs_mnttab_fini(g_zfs);
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "libzfs_mnttab_fini DONE\n");
            libzfs_mnttab_init(g_zfs);
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "libzfs_mnttab_init DONE\n");
            libzfs_mnttab_cache(g_zfs, B_TRUE);
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "libzfs_mnttab_cache DONE\n");
            strcat(rdataset,"/");
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "DEBUG : BEFORE bin2hex\n");
            if(bin2hex(ds, readbuf)==0) { 
              LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "DEBUG : AFTER bin2hex\n");
              LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Error during hexadecimal conversion\n");
              strcpy(writebuf, "1:bin2hex(new)");
              goto GTSyncEnd;
            }
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Dataset will be put in the buffer as %s\n", readbuf);
            strcat(rdataset,readbuf);
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW:BUFFERED) on fd => START (rdataset:%s)\n", rdataset);
          }
          if(zfs_receive(g_zfs, rdataset, NULL, &recv_flags, fd, NULL)==0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW) => OK\n");
            /* 
             * -- Following code does not work on OpenZFS : https://github.com/openzfs/zfs/pull/11608
             * -- We have to send 2 streams : First from origin to firstsnap, the other from firstsnap to lastsnap with all the snapshots between
             * strcpy(writebuf, "0:OK");
             */
            is_incremental=0;
            goto GTSyncInc; 
          } else {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (NEW) => ERROR\n");
            strcpy(writebuf, "1:zfs_receive(new)");
          }
        } else if (strcmp(writebuf, "0:FULL")==0) {
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) on fd => START (rdataset:%s)\n", rdataset);
          recv_flags.force=B_TRUE;
          //if (strcmp(ds,"")==0) {
          recv_flags.isprefix=B_FALSE;
          recv_flags.istail=B_TRUE;
          //}
          //recv_flags.force=B_TRUE;
          //Then we prepare to receive the stream
          //Extracting the parent dataset name
          //strcat(rdataset,ds);
          //ptr = strrchr(rdataset, '/');
          //bzero(ptr,1);
          //LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) on fd : rdataset:%s\n", rdataset);
          if(zfs_receive(g_zfs, rdataset, NULL, &recv_flags, fd, NULL)==0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) => OK\n");
            /*
             * -- Following code does not work on OpenZFS : https://github.com/openzfs/zfs/pull/11608
             * -- We have to send 2 streams : First from origin to firstsnap, the other from firstsnap to lastsnap with all the snapshots between
             * strcpy(writebuf, "0:OK");
             */
            is_incremental=0;
            goto GTSyncInc;
          } else {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) => ERROR\n");
            strcpy(writebuf, "1:zfs_receive(full)");
          }
          //LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (FULL) : zfs_close\n", fd, ds, cmd, rdataset);
          //zfs_close(zhp);
        }else{
GTSyncInc:
          recv_flags.isprefix=B_FALSE;
          recv_flags.istail=B_FALSE;
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (INCR) on fd => START (rdataset:%s)\n", rdataset);
          //Then we prepare to receive the stream
          if(zfs_receive(g_zfs, rdataset, NULL, &recv_flags, fd, NULL)==0) {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (INCR) => OK\n");
          } else {
            LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (INCR) => ERROR\n");
            strcpy(writebuf, "1:zfs_receive(incr)");
            goto GTSyncEnd;
          }
          //We delete snapshots that are not available on the sender
          if(is_incremental==0) {
            strcpy(writebuf, "0:OK");
            goto GTSyncEnd;
          }
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "Cleaning useless snapshots\n");
          zhp = zfs_open(g_zfs, rdataset, ZFS_TYPE_FILESYSTEM);
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
          LogItThread(tLogFile, tcMS, tcDT, fd, ds, cmd, "zfs_receive (INCR) : zfs_close\n", fd, ds, cmd, rdataset);
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
    //boolean_t verbose = B_FALSE;
    struct rusage memory;

    while ((c = getopt(argc, argv, ":vp:i:b:t:o:d:")) != -1) {
        switch (c) {
        case 'v':
            Verbose = B_TRUE;
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
    
    //Should be
    if ((G_zfs = libzfs_init()) == NULL) {
      LogItMain("Unable to init libzfs\n");
      exit(1);
    }
  
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

