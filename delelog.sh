#!/bin/bash

# 日志目录
LOG_DIR="/var/log/myapp"

# 保留天数
DAYS=7

# 日志文件匹配规则
PATTERN="*.log"

# 先判断目录是否存在
if [ ! -d "$LOG_DIR" ]; then
    echo "$(date '+%F %T') [ERROR] directory not found: $LOG_DIR"
    exit 1
fi

# 删除超过 DAYS 天的日志
find "$LOG_DIR" -type f -name "$PATTERN" -mtime +$DAYS -print -exec rm -f {} \;

echo "$(date '+%F %T') [INFO] cleanup finished in $LOG_DIR"