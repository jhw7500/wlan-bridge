#!/bin/bash

# Development script to copy source/Makefile to target
# Updated for wbridge nomenclature

scp ../wbridge/Makefile root@192.168.0.110:/root/claude
scp ../wbridge/wbridge-pcap.c root@192.168.0.110:/root/claude
scp ../wbridge/wbridge-tpacket.c root@192.168.0.110:/root/claude
