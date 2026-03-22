const path = require('node:path');

module.exports = {
  packagerConfig: {
    icon: path.join(__dirname, 'assets', 'icon'),
    asar: {
      unpack: '**/*.node'
    }
  },
  makers: [
    {
      name: '@electron-forge/maker-zip',
      platforms: ['darwin']
    },
    {
      name: '@electron-forge/maker-dmg',
      config: {
        icon: path.join(__dirname, 'assets', 'icon.icns')
      }
    },
    {
      name: '@electron-forge/maker-squirrel'
    }
  ]
};
