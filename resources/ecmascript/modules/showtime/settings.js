var prop  = require('showtime/prop');
var store = require('showtime/store');


function createSetting(group, type, conf) {

  if(typeof(conf.title) != 'string')
    throw 'Title not set'

  if(typeof(conf.id) != 'string')
    throw 'Id not set'

  var model = group.model.nodes[conf.id];

  model.type = type;
  model.enabled = true;
  model.metadata.title = conf.title;


  var item = {};

  Object.defineProperties(item, {
    model: {
      value: model
    },

    vale: {
      get: function() {
        return model.value;
      }
    },

    enabled: {
      set: function(val) {
        model.enabled = val;
      },
      get: function() {
        return parseInt(model.enabled) ? true : false;
      }
    }
  });

  return item;
}



exports.globalSettings = function(id, title, icon, desc) {

  Showtime.fs.mkdirs('settings');

  this.id = id;
  this.model = prop.global.settings.apps.nodes[id];
  this.model.type = 'settings';
  this.model.url = prop.makeUrl(this.model);

  var metadata = this.model.metadata;

  metadata.title = title;
  metadata.icon = icon;
  metadata.shortdesc = desc;

  var mystore = store.createFromPath('settings/' + id);

  this.getvalue = function(conf) {
    return conf.id in mystore ? mystore[conf.id] : conf.def;
  };

  this.setvalue = function(conf, value) {
    mystore[conf.id] = value;
  };
}

var sp = exports.globalSettings.prototype;

/// -----------------------------------------------
/// Bool
/// -----------------------------------------------

sp.destroy = function() {
  this.zombie = 1;
  delete prop.global.settings.apps.nodes[this.id];
}


/// -----------------------------------------------
/// Bool
/// -----------------------------------------------

sp.createBool = function(conf) {
  var group = this;
  var item = createSetting(group, 'bool', conf);

  item.model.value = group.getvalue(conf);

  prop.subscribeValue(item.model.value, function(newval) {
    if(group.zombie)
      return;

    group.setvalue(conf, newval);
    if('callback' in conf)
      conf.callback(newval);
  }, {
    ignoreVoid: true,
    autoDestroy: true
  });
  return item;
}

/// -----------------------------------------------
/// String
/// -----------------------------------------------

sp.createString = function(conf) {
  var group = this;
  var item = createSetting(group, 'string', conf);

  item.model.value = group.getvalue(conf);

  prop.subscribeValue(item.model.value, function(newval) {
    if(group.zombie)
      return;

    group.setvalue(conf, newval);
    if('callback' in conf)
      conf.callback(newval);
  }, {
    ignoreVoid: true,
    autoDestroy: true
  });

  return item;
}

/// -----------------------------------------------
/// Integer
/// -----------------------------------------------

sp.createInteger = function(conf) {
  var group = this;

  var item = createSetting(group, 'integer', conf);

  item.model.value = group.getvalue(conf);

  item.model.min  = conf.min;
  item.model.max  = conf.max;
  item.model.step = conf.step;
  item.model.unit = conf.unit;

  prop.subscribeValue(item.model.value, function(newval) {
    if(group.zombie)
      return;

    newval = parseInt(newval);
    group.setvalue(conf, newval);
    if('callback' in conf)
      conf.callback(newval);
  }, {
    ignoreVoid: true,
    autoDestroy: true
  });

  return item;
}


/// -----------------------------------------------
/// Divider
/// -----------------------------------------------

sp.createDivider = function(conf) {
  var group = this;
  var node = prop.createRoot();
  node.type = 'separator';
  node.metadata.title = conf.title;
  prop.setParent(node, group.model.nodes);
}


/// -----------------------------------------------
/// Info
/// -----------------------------------------------

sp.createInfo = function(conf) {
  var group = this;
  var node = prop.createRoot();
  node.type = 'info';
  node.description = conf.description;
  node.image = conf.image;
  prop.setParent(node, group.model.nodes);
}


/// -----------------------------------------------
/// Action
/// -----------------------------------------------

sp.createAction = function(conf) {
  var group = this;

  var item = createSetting(group, 'action', conf);

  prop.subscribe(item.model.action, function(type) {
    if(type == 'event' && 'callback' in conf)
      conf.callback();
  }, {
    autoDestroy: true
  });

  return item;
}

