/*
 Navicat Premium Data Transfer

 Source Server         : k8s-读写分离-主
 Source Server Type    : MariaDB
 Source Server Version : 100134
 Source Host           : 10.254.78.30:3306
 Source Schema         : proxy_heart_beat

 Target Server Type    : MariaDB
 Target Server Version : 100134
 File Encoding         : 65001

 Date: 18/07/2018 10:36:09
*/

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ----------------------------
-- Table structure for tb_heartbeat
-- ----------------------------
DROP TABLE IF EXISTS `tb_heartbeat`;
CREATE TABLE `tb_heartbeat` (
  `p_id` varchar(128) NOT NULL,
  `p_ts` timestamp(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`p_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

SET FOREIGN_KEY_CHECKS = 1;
