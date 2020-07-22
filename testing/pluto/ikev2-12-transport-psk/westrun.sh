ipsec auto --up ipv4-psk-ikev2-transport
../../pluto/bin/ping-once.sh --up -I 192.1.2.45 192.1.2.23
ipsec whack --trafficstatus
# test rekey
ipsec whack --rekey-ipsec --name ipv4-psk-ikev2-transport
# confirm transport mode is still part after rekey
ip xfrm state |grep mode
echo done
