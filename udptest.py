
import socket
import time

UDP_IP = "sgmk2.iot"
UDP_PORT = 1234

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

order = 1
parameter = 250 + 32768    # tilt =1   0 - 480
print(bytes([order])+parameter.to_bytes(2,'big'))
sock.sendto(bytes([order])+parameter.to_bytes(2,'big'), (UDP_IP, UDP_PORT))

# order = 3
# parameter = -100 + 32768
# print(bytes([order])+parameter.to_bytes(2,'big'))
# sock.sendto(bytes([order])+parameter.to_bytes(2,'big'), (UDP_IP, UDP_PORT))
