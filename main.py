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
    
    subprocess.run(f"./mrx {ip} {port} {duration} {threads}", shell=True)

if __name__ == "__main__":
    main()