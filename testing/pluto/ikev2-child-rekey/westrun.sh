# bring up west and confirm
ipsec auto --up west
../../pluto/bin/ping-once.sh --up -I 192.0.1.254 192.0.2.254
# expect IKE #1 CHILD #2
ipsec whack --trafficstatus
# why?
echo "sleep 9"
sleep 9
# rekey CHILD SA
ipsec whack --rekey-ipsec --name west
sleep 2
# expect IKE #1 CHILD #3
ipsec status |grep STATE_
../../pluto/bin/ping-once.sh --up -I 192.0.1.254 192.0.2.254
ipsec whack --trafficstatus
# why?
echo "sleep 11"
sleep 11
../../pluto/bin/ping-once.sh --up -I 192.0.1.254 192.0.2.254
# rekey CHILD SA
ipsec whack --rekey-ipsec --name west
sleep 2
# expect IKE #1 CHILD #4
ipsec status |grep STATE_
../../pluto/bin/ping-once.sh --up -I 192.0.1.254 192.0.2.254
ipsec whack --trafficstatus
echo done
