import socket,json,sys,time
s=socket.socket(socket.AF_UNIX); 
for i in range(50):
    try: s.connect("/tmp/mpvsock"); break
    except Exception: time.sleep(0.2)
time.sleep(3)
def cmd(*a):
    s.send((json.dumps({"command":list(a)})+"\n").encode()); time.sleep(0.5)
cmd("screenshot-to-file",sys.argv[1],"window")
time.sleep(1); cmd("quit")
