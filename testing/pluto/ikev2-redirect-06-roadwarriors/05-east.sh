sleep 2
# both clients should be connected now
ipsec whack --trafficstatus
# send REDIRECT in informational to all tunnels from connection east-any (north and road)
ipsec whack --name east-any --redirect-to 192.1.2.45
# give them time to be redirected
sleep 2
# both should be gone now
