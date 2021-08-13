iptables -t nat -F
iptables -F
# NAT to NIC's address
# NAT UDP 500,4500 to NICs address with sport
iptables -t nat -A POSTROUTING -s 192.1.3.0/24 -p udp --sport 4500  -j SNAT --to-source 192.1.2.254:54500-54700
iptables -t nat -A POSTROUTING -s 192.1.3.0/24 -p udp --sport 500  -j SNAT --to-source 192.1.2.254:50500-50700
iptables -t nat -A POSTROUTING -s 192.1.3.0/24 -j SNAT --to-source 192.1.2.254
