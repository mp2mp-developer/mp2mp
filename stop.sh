#!/bin/bash

ldpd=`ps -ef | grep 'ldpd -d' | grep -v grep | awk '{print $2}'`
if [ $ldpd ]
then
    sudo kill -9 $ldpd
else
    echo "no process name ldpd!"
fi

zebra=`ps -ef | grep 'zebra' | grep -v grep | awk '{print $2}'`
if [ $zebra ]
then
	sudo kill -9 $zebra
else
	echo "no trocess name zebra!"
fi

ospf=`ps -ef | grep 'ospf' | grep -v grep | awk '{print $2}'`
if [ $ospf ]
then
	sudo kill -9 $ospf
else
	echo "no process name ospf!"
fi
