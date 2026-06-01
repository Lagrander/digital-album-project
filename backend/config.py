# ============================================================
# 云端电子相册 — 配置文件
# 基于 InkTime config 改造，适配 800×480 彩色 LCD + 云端存储
# ============================================================

# --- 照片存储路径（云端/本地/NAS 挂载目录） ---
IMAGE_DIR = r"W:\Desktop\Smart_digital_photo_album\photos"

# --- 数据库路径 ---
DB_PATH = "./photos.db"

# --- 上传照片数据库 ---
UPLOAD_DB_PATH = "./upload.db"

# --- 上传照片原图存储目录 ---
UPLOAD_DIR = "./upload_photos"

# VLM 分析配置（与 InkTime 完全一致）

API_CHANNELS = [
    {
        "api_url":    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
        "api_key":    "sk-97cae9f2a0a54f83bce650d520988fbe",
        "model_name": "qwen-vl-max",
    },
]

BATCH_LIMIT = None
TIMEOUT = 600
CHANNEL_FAILOVER_COOLDOWN_SEC = 300

# 服务器配置

DOWNLOAD_KEY = "yourdownloadkey"      # ESP32 下载路径令牌（改复杂点）
SURPRISE_UPLOAD_PASSWORD = ""         # 惊喜上传密码（空=公开访问，适合家庭使用）
FLASK_HOST = "0.0.0.0"
FLASK_PORT = 8765
ENABLE_REVIEW_WEBUI = True

# GPS 城市解析 + 旅行加分

WORLD_CITIES_CSV = "./data/world_cities_zh.csv"
CITY_GRID_DEG = 1.0
HOME_LAT = 22.543096
HOME_LON = 114.057865
HOME_RADIUS_KM = 60.0
CITY_MAX_DISTANCE_KM = 100.0

# 渲染配置（适配 800×480 彩色 LCD）

BIN_OUTPUT_DIR = "./output"
FONT_PATH = ""

# 每日选片阈值
MEMORY_THRESHOLD = 70.0
DAILY_PHOTO_QUANTITY = 5

# LCD 屏幕尺寸（竖屏 480×800）
LCD_WIDTH = 480
LCD_HEIGHT = 800
TEXT_AREA_HEIGHT = 0

# 百度网盘云端同步与 AI 处理配置
BAIDU_ACCESS_TOKEN = "121.0eca91a12139d39d3fdf9576e2ee45ab.Y5pqA7y-bOxRWKte6SeDa9hzYPSV7GFiX9pc1z-.VOt8LQ"
BAIDU_REMOTE_DIR = "/apps/digital_album"
OLLAMA_MODEL = "llava:7b"

