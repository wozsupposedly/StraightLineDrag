module.exports = {
  packagerConfig: {
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
      name: '@electron-forge/maker-dmg'
    },
    {
      name: '@electron-forge/maker-squirrel'
    }
  ]
};
