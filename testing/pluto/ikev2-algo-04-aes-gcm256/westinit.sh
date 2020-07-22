/testing/guestbin/swan-prep
# confirm that the network is alive
../../pluto/bin/wait-until-alive 192.0.2.254
# ensure that clear text does not get through
iptables -A INPUT -i eth1 -s 192.0.2.0/24 -j DROP
iptables -A INPUT -i eth1 -s 192.0.2.0/24 -j DROP
iptables -I INPUT -m policy --dir in --pol ipsec -j ACCEPT
# confirm clear text does not get through
ping -n -c 4 192.0.2.254
ipsec start
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-ipv4-psk-ikev2-gcm-c
ipsec auto --status
ipsec whack --impair suppress-retransmits
echo "initdone"
