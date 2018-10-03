ip link add name lo1 type dummy 
ip link set dev lo1 up 
ip addr add 7.7.7.7/32 dev lo1
modprobe mpls_router 
modprobe mpls_gso 
modprobe mpls_iptunnel 
sysctl -w net.mpls.conf.ens33.input=1 
sysctl -w net.mpls.conf.lo.input=1 
sysctl -w net.mpls.conf.lo1.input=1
sysctl -w net.mpls.platform_labels=1048575
sysctl -w net.ipv4.ip_forward=1
sysctl -w net.ipv6.conf.all.forwarding=1
route add -net 9.9.9.9 netmask 255.255.255.255 gw 192.168.6.134 dev ens33
route add -net 8.8.8.8 netmask 255.255.255.255 gw 192.168.6.134 dev ens33
route add -net 6.6.6.6 netmask 255.255.255.255 gw 192.168.6.134 dev ens33
#route add -net 7.7.7.7 netmask 255.255.255.255 gw 192.168.6.133 dev ens33
