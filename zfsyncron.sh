#!/bin/sh
#############
# zfsyncron.sh
# creates snapshot on the Dataset to be synced and its children (and destroy the oldest ones, according to retention settings)
#
# This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover
# Twitter : @cedricmaunoury
# Linkedin : cedric-maunoury
#############

PATH=/sbin:/bin:/usr/bin:/usr/local/bin

ZFSYNC_ERROR=0

display_help () {
  echo "This script should have at least 2 parameters : The dataset to be synced and a remote computer"
  echo "zfsyncron.sh DATASET COMPUTER1 COMPUTER2 ..."
  echo "OPTIONS :"
  echo "-m : Local retention for Minutely snapshots"
  echo "-h : Local retention for Hourly snapshots" 
  echo "-d : Local retention for Daily snapshots"
  echo "-p : Remote TCP Port to connect to"
  echo "-t : Number of thread to send the ZFS streams"
  echo "-H : Display this help"
}

cd `dirname "$0"`
#Default retention
m=4
h=3
d=2
p=30
t=2
#Snapshot !
echo "================="
snapname=`date "+%Y%m%d-%H%M"`
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
while getopts m:h:d:p:t:H: OPT
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
    H)
       display_help
       exit 0;;
  esac
done
shift $((OPTIND-1))
if [ $# -le 1 ]
then
  display_help
  exit 1
fi

RootDataset="$1"
echo "RootDataset : $RootDataset"
shift
zfs snapshot -r $RootDataset@$snapname
if [ $? -ne 0 ]
then
  echo "Snapshot creation failed (${RootDataset}@${snapname})"
  exit 2
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
  echo zfsync_send -p $p $RootDataset $IP
  ./send/zfsync_send -p $p -t $t $RootDataset $IP
  ERROR=$?
  echo "RC : "$ERROR
  if [ $ERROR -ne 0 ]
  then
    ZFSSYNC_ERROR=3
  fi
done
echo "================="
exit $ZFSSYNC_ERROR
