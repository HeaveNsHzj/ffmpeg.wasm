const fs = require('fs');
const path = require('path');
const { exec } = require('child_process');

const ROOT = "public/assets";
const mkdir = (dir) => {
  !fs.existsSync(dir) && fs.mkdirSync(dir, {
    recursive: true
  });
};

const copyDir = async (package) => {
  const dir = `${ROOT}/${package}/package`;
  // if (fs.existsSync(dir)) {
  //   console.log(`found @ffmpeg/${package} assets.`);
  //   return;
  // }
  mkdir(dir);
  const cwd = process.cwd();
  const from = path.resolve(cwd, '../../packages', package, 'dist');
  const to = `./${ROOT}/${package}/package/`;
  console.log('cp', from, to);
  exec("cp -r " +  from + ' ' + to, (error) => {
    if (error) {
      console.error(error);
    }
  });
};
mkdir(ROOT);
copyDir('ffmpeg');
copyDir('util');
copyDir('core');
//copyDir('core-mt');



