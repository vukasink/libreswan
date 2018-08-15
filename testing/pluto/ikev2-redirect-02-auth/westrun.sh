ipsec auto --up  westnet-eastnet-ipv4-psk-ikev2
ping -n -c 4 -I 192.0.1.254 192.0.2.254
ipsec whack --trafficstatus
# there should really be only two states, but on the first connection
# we requested a SPI from the kernel and by default it takes 300s for
# that to time out, so we see two real states and a larval state for
# a total of 3
ip -o xfrm state | wc -l
echo done
