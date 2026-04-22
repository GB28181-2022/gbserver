#!/bin/bash
cd /home/user/coder/gb_service2022/backend/build
pkill -9 gb_service 2>/dev/null
sleep 1
./gb_service > /tmp/gb_service.log 2>&1 &
sleep 3
echo "Service started"
curl -s http://192.168.1.9:8080/api/cameras?page=1&pageSize=1 | head -1
