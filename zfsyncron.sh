#!/bin/sh
#############
# zfsyncron.sh
# creates snapshot on the Dataset to be synced and its children (and destroy the oldest ones, according to retention settings)
#
# This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover
# Twitter : @cedricmaunoury
# Linkedin : cedric-maunoury
#############

## Usefull var

# Local retention for Minutely snapshots
m=4

# Local retention for Hourly snapshots
h=3

# Local retention for Daily snapshots
d=2

# Remote TCP Port to connect to
p=30

# Number of thread to send the ZFS streams
t=2

# Network used to send
i=127.0.0.1

# Log file name
o=""

# Snapshot name
snapname=`date "+%Y%m%d-%H%M"`

# PID file
pidfile=/var/run/zfsyncron.pid

## Fixed var
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin:$(dirname $0)/send
ZFSYNC_ERROR=0

## Function

display_help () {
  echo "This script should have at least 2 parameters : The dataset to be synced and a remote computer"
  echo "zfsyncron.sh DATASET COMPUTER1 COMPUTER2 ..."
  echo "OPTIONS :"
  echo "-m : Local retention for Minutely snapshots (default: 4)"
  echo "-h : Local retention for Hourly snapshots (default: 3)" 
  echo "-d : Local retention for Daily snapshots (default: 2)"
  echo "-p : Remote TCP Port to connect to (default: 30)"
  echo "-t : Number of thread to send the ZFS streams (default: 2)"
  echo "-i : Network used to send (default: 127.0.0.1)"
  echo "-o : Log file name"
  echo "-H : Display this help"
}

death () {
  rm $pidfile
  exit $1
}

## Begin the script

if [ -f $pidfile ]
then
  echo "Don't. Cross. The streams. It would be bad."
  exit 4
fi

$$ > $pidfile
cd `dirname "$0"`
#Snapshot !
echo "================="
echo $snapname
echo "-----------------"
snapext="M"
if [ `echo -n $snapname | tail -c4` == "0300" ]
then
  snapext="D"
elif [ `echo -n $snapname | tail -c2` == "00" ]
then
  snapext="H"
fi
snapname=$snapname"_"$snapext
echo "Snapshot name : "$snapname
while getopts m:h:d:p:t:i:o:H: OPT
do
  case $OPT in
    m)
       m=$OPTARG;;
    h)
       h=$OPTARG;;
    d)
       d=$OPTARG;;
    p)
       p=$OPTARG;;
    t)
       t=$OPTARG;;
    i)
       i=$OPTARG;;
    o)
       o="-o $OPTARG";;
    H)
       display_help
       death 0;;
  esac
done
shift $((OPTIND-1))
if [ $# -le 1 ]
then
  display_help
  death 1
fi

RootDataset="$1"
echo "RootDataset : $RootDataset"
shift
zfs snapshot -r $RootDataset@$snapname
if [ $? -ne 0 ]
then
  echo "Snapshot creation failed (${RootDataset}@${snapname})"
  death 2
fi
echo "Snapshot done (${RootDataset}@${snapname})"
echo "Minutely : $m"
echo "Hourly : $h"
echo "Daily : $d"
echo "Deleting snapshots according to defined retention"
cpt=0
case $snapext in 
  M) cpt=$(($m+0));;
  H) cpt=$(($h+0));;
  D) cpt=$(($d+0));;
esac
echo "Retention for '$snapext' extension : $cpt"
for snap in `zfs list -H -r -d1 -o name -t snapshot $RootDataset | egrep "_${snapext}$" | sort -r`
do
  if [ $cpt -ge 1 ]
  then
    cpt=$(($cpt-1))
    echo "Keeping ${snap}"
  else
    echo "Destroying ${snap}"
    zfs destroy -r $snap
  fi
done
echo "Time to send last diff to my friends"
#set -x
for IP in "$@"
do
  echo "======"$IP"======"
  echo zfsync_send -v -p $p -i $i -t $t $o $RootDataset $IP
  zfsync_send -v -p $p -i $i -t $t $o $RootDataset $IP
  ERROR=$?
  echo "RC : "$ERROR
  if [ $ERROR -ne 0 ]
  then
    ZFSSYNC_ERROR=3
  fi
done
echo "================="
death $ZFSSYNC_ERROR
