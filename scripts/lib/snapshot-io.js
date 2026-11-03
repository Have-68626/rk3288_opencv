"use strict";

// 仅在单次只读盘点中共享元数据；后续清理和恢复必须重新读取文件系统。
function createSnapshotIO(io = require('node:fs/promises')) {
  const directories=new Map();
  return {
    readdir(file,options) {
      const key=JSON.stringify([String(file),options??null]);
      if(!directories.has(key))directories.set(key,io.readdir(file,options));
      return directories.get(key);
    },
    // 实测没有重复stat收益，避免为整个文件树额外保留Stats对象。
    lstat(...args) { return io.lstat(...args); },
  };
}

module.exports={createSnapshotIO};
