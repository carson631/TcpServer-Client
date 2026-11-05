-- TCP 聊天服务器数据库结构
-- MySQL 8.0 / Ubuntu 24.04
--
-- 用法（在 Ubuntu 上以 root 执行）：
--   sudo mysql < sql/schema.sql
--
-- 程序启动时也会自动建表（CREATE TABLE IF NOT EXISTS），
-- 这份脚本主要用于手动初始化或迁移到新机器。

CREATE DATABASE IF NOT EXISTS tcp_chat
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE tcp_chat;

-- 用户：只存密码的 SHA-256 摘要，不存明文
-- online 是“当前是否在线”的标记，由服务器在登录/登出/断线时维护；
-- 服务器每次启动会把整列清零，避免上次崩溃留下脏数据。
CREATE TABLE IF NOT EXISTS users (
  id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
  username   VARCHAR(32)  NOT NULL,
  pass_hash  CHAR(64)     NOT NULL,
  online     TINYINT(1)   NOT NULL DEFAULT 0,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 私聊消息：每发一条就落一行，供管理端统计与追溯
CREATE TABLE IF NOT EXISTS messages (
  id        BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  sender    VARCHAR(32)   NOT NULL,
  receiver  VARCHAR(32)   NOT NULL,
  content   VARCHAR(2000) NOT NULL,
  sent_at   DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_sender (sender),
  KEY idx_receiver (receiver),
  KEY idx_sent (sent_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 好友关系：双向各存一行，(user_a, user_b) 作主键天然去重
CREATE TABLE IF NOT EXISTS friends (
  user_a     VARCHAR(32) NOT NULL,
  user_b     VARCHAR(32) NOT NULL,
  created_at DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_a, user_b)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 应用账号：只给 tcp_chat 库的权限，不给全局权限
CREATE USER IF NOT EXISTS 'chat'@'%' IDENTIFIED BY 'chat123456';
ALTER USER 'chat'@'%' IDENTIFIED BY 'chat123456';
GRANT ALL PRIVILEGES ON tcp_chat.* TO 'chat'@'%';
FLUSH PRIVILEGES;
