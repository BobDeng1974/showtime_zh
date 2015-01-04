var prop = require('showtime/prop');
var settings = require('showtime/settings');

// ---------------------------------------------------------------
// The Item object
// ---------------------------------------------------------------

function Item(page) {
  Object.defineProperties(this, {

    root: {
      value: prop.createRoot()
    },
    page: {
      value: page
    }
  });
  this.eventhandlers = {};
}

Duktape.fin(Item.prototype, function(x) {
  if(this.mlv)
    Showtime.videoMetadataUnbind(this.mlv);
});

Item.prototype.bindVideoMetadata = function(obj) {
  if(this.mlv)
    Showtime.videoMetadataUnbind(this.mlv);
  this.mlv = Showtime.videoMetadataBind(this.root, this.root.url, obj);
}

Item.prototype.toString = function() {
  return '[ITEM with title: ' + this.root.metadata.title.toString() + ']';
}

Item.prototype.dump = function() {
  prop.print(this.root);
}

Item.prototype.enable = function() {
  this.root.enabled = true;
}

Item.prototype.disable = function() {
  this.root.enabled = false;
}

Item.prototype.addOptAction = function(title, action) {
  var node = prop.createRoot();
  node.type = 'action';
  node.metadata.title = title;
  node.enabled = true;
  node.action = action;

  prop.setParent(node, this.root.options);
}


Item.prototype.addOptURL = function(title, url) {
  var node = prop.createRoot();
  node.type = 'location';
  node.metadata.title = title;
  node.enabled = true;
  node.url = url;

  prop.setParent(node, this.root.options);
}


Item.prototype.addOptSeparator = function(title) {
  var node = prop.createRoot();
  node.type = 'separator';
  node.metadata.title = title;
  node.enabled = true;

  prop.setParent(node, this.root.options);
}


Item.prototype.destroy = function() {
  var pos = this.page.items.indexOf(this);
  if(pos != -1)
    this.page.items.splice(pos, 1);
  prop.destroy(this.root);
}


Item.prototype.moveBefore = function(before) {
  prop.moveBefore(this.root, before ? before.root : null);
  var thispos = this.page.items.indexOf(this);
  if(before) {
    var beforepos = this.page.items.indexOf(before);
    this.page.items.splice(thispos, 1);
    if(beforepos > thispos)
      beforepos--;
    this.page.items.splice(beforepos, 0, this);
  } else {
    this.page.items.splice(thispos, 1);
    this.page.items.push(this);
  }
}



Item.prototype.onEvent = function(type, callback) {
  if(type in this.eventhandlers) {
    this.eventhandlers[type].push(callback);
  } else {
    this.eventhandlers[type] = [callback];
  }

  if(!this.eventSubscription) {
    this.eventSubscription =
      prop.subscribe(this.root, function(type, val) {
        if(type != "action")
          return;
        if(val in this.eventhandlers) {
          for(x in this.eventhandlers[val]) {
            this.eventhandlers[val][x](val);
          }
        }

    }.bind(this), {
      autoDestroy: true
    });
  }
}


// ---------------------------------------------------------------
// The Page object
// ---------------------------------------------------------------

function Page(root, sync, flat) {
  this.items = [];
  this.sync = sync;
  this.root = root;
  this.model = flat ? this.root : this.root.model;
  this.root.entries = 0;
  this.eventhandlers = [];

  var model = this.model;

  Object.defineProperties(this, {

    type: {
      get: function()  { return model.type; },
      set: function(v) { model.type = v; }
    },

    metadata: {
      get: function()  { return model.metadata; }
    },

    loading: {
      get: function()  { return model.loading; },
      set: function(v) { model.loading = v; }
    },

    source: {
      get: function()  { return root.source; },
      set: function(v) { root.source = v; }
    }
  });

  if(!flat) {
    this.options = new settings.kvstoreSettings(this.model.options,
                                                this.root.url,
                                                'plugin');
  }

  this.nodesub =
    prop.subscribe(model.nodes, function(op, value, value2) {
      if(op == 'wantmorechilds') {
        var nodes = model.nodes;
        var have_more = false;

        if(typeof this.paginator == 'function') {

          try {
            have_more = !!this.paginator();
          } catch(e) {
            if(!prop.isZombie(model)) {
              throw e;
            } else {
              console.log("Page closed during pagination, error supressed");
            }
          }
        }
        Showtime.propHaveMore(nodes, have_more);
      }

      if(op == 'reqmove' && typeof this.reorderer == 'function') {
        var item = this.items[this.findItemByProp(value)];
        var before = value2 ? this.items[this.findItemByProp(value2)] : null;
        this.reorderer(item, before);
      }
    }.bind(this), {
      autoDestroy: true
    });
}

Page.prototype.findItemByProp = function(v) {
  for(var i = 0; i < this.items.length; i++) {
    if(prop.isSame(this.items[i].root, v)) {
      return i;
    }
  }
  return -1;
}

Page.prototype.error = function(msg) {
  this.model.loading = false;
  this.model.type = 'openerror';
  this.model.error = msg.toString();
}

Page.prototype.getItems = function() {
  return this.items.slice(0);
}


Page.prototype.appendItem = function(url, type, metadata) {
  this.root.entries++;

  var item = new Item(this);
  this.items.push(item);

  var root = item.root;
  root.url = url;
  root.type = type;
  root.metadata = metadata;
  Showtime.propSetParent(root, this.model.nodes);
  return item;
}

Page.prototype.appendAction = function(type, data, enabled, metadata) {
  var item = new Item(this);

  var root = item.root;
  root.enabled = enabled;
  root.type = type;
  root.data = data;
  root.metadata = metadata;
  Showtime.propSetParent(root, this.model.actions);
  return item;
}

Page.prototype.appendPassiveItem = function(type, data, metadata) {
  this.root.entries++;

  var item = new Item(this);
  this.items.push(item);

  var root = item.root;
  root.type = type;
  root.data = data;
  root.metadata = metadata;
  Showtime.propSetParent(root, this.model.nodes);
  return item;
}

Page.prototype.dump = function() {
  Showtime.propPrint(this.root);
}

Page.prototype.flush = function() {
  prop.deleteChilds(this.model.nodes);
}

Page.prototype.redirect = function(url) {

  Showtime.resourceDestroy(this.nodesub);

  if(this.sync) {
    Showtime.backendOpen(this.root, url, true);
  } else {
    prop.sendEvent(this.root.eventSink, "redirect", url);
  }
}

Page.prototype.onEvent = function(type, callback) {
  if(type in this.eventhandlers) {
    this.eventhandlers[type].push(callback);
  } else {
    this.eventhandlers[type] = [callback];
  }

  if(!this.eventSubscription) {
    this.eventSubscription =
      prop.subscribe(this.root.eventSink, function(type, val) {
        if(type != "action")
          return;
        if(val in this.eventhandlers) {
          for(x in this.eventhandlers[val]) {
            this.eventhandlers[val][x](val);
          }
        }

    }.bind(this), {
      autoDestroy: true
    });
  }
}


// ---------------------------------------------------------------
// ---------------------------------------------------------------
// Exported functions
// ---------------------------------------------------------------
// ---------------------------------------------------------------


exports.Route = function(re, callback) {

  this.route = Showtime.routeCreate(re, function(pageprop, sync, args) {

    try {

      // First, convert the raw page prop object into a proxied one
      pageprop = prop.makeProp(pageprop);

      // Prepend a Page object as first argument to callback
      args.unshift(new Page(pageprop, sync, false));

      callback.apply(null, args);
    } catch(e) {

      if(!prop.isZombie(pageprop)) {
        throw e;
      } else {
        console.log("Page at route " + re + " was closed, error supressed");
      }
    }
  });
}

exports.Route.prototype.destroy = function() {
  Showtime.resourceDestroy(this.route);
}


exports.Searcher = function(title, icon, callback) {

  this.searcher = Showtime.hookRegister('searcher', function(model, query, loading) {

    try {

      // Convert the raw page prop object into a proxied one
      model = prop.makeProp(model);

      var root = prop.createRoot();

      root.metadata.title = title;
      root.metadata.icon = icon;
      root.type = 'directory';
      prop.setParent(root, model.nodes);

      var page = new Page(root, false, true);
      page.type = 'directory';
      root.url = Showtime.propMakeUrl(page.root);
      prop.atomicAdd(loading, 1);
      try {
        callback(page, query);
      } finally {
        prop.atomicAdd(loading, -1);
      }
    } catch(e) {

      if(!prop.isZombie(model)) {
        throw e;
      } else {
        console.log("Search for " + query + " was closed, error supressed");
      }
    }
  });
}



exports.Searcher.prototype.destroy = function() {
  Showtime.resourceDestroy(this.searcher);
}
