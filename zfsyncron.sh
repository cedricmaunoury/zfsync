#!/bin/sh
PATH=/sbin:/bin:/usr/bin:/usr/local/bin
cd `dirname "$0"`
if [ $# -le 1 ]
then
  echo "This script should have at least 2 parameters : The dataset to be synced and a remote IP"
  echo "zfsyncron.sh DATASET IP1 IP2 ..."
  echo "OPTIONS :"
  echo "-m : Local retention for Minutely snapshots"
  echo "-h : Local retention for Hourly snapshots" 
  echo "-d : Local retention for Daily snapshots"
  echo "-p : Remote TCP Port to connect to"
  echo "-t : Number of thread to send the ZFS streams"
  exit 1
fi
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
while getopts m:h:d:p: OPT
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
  esac
done
shift $((OPTIND-1))
if [ $# -le 1 ]
then
  echo "This script should have at least 2 parameters : The dataset to be synced and a remote IP"
  echo "zfsyncron.sh DATASET IP1 IP2 ..."
  echo "OPTIONS :"
  echo "-m : Local retention for Minutely snapshots"
  echo "-h : Local retention for Hourly snapshots"
  echo "-d : Local retention for Daily snapshots"
  echo "-p : Remote TCP Port to connect to"
  exit 1
fi

RootDataset="$1"
echo "RootDataset : $RootDataset"
shift
zfs snapshot -r $RootDataset/local@$snapname
if [ $? -ne 0 ]
then
  echo "Snapshot creation failed ($RootDataset/local@$snapname)"
  exit 1
fi
echo "Snapshot done ($RootDataset/local@$snapname)"
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
for snap in `zfs list -H -r -d1 -o name -t snapshot $RootDataset/local | egrep "_${snapext}$" | sort -r`
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
  echo "RC : "$?
done
echo "================="
