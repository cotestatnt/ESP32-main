// We use event source from server for start websocket
// In that mode we can use auto-reconnect of event source also for websocket
if (!!window.EventSource) {
  var source = new EventSource('/events');
  source.addEventListener('open', function(e) {
    console.log("Server Events connected, start Websocket client.");
    startSocket();
  }, false);
 
  source.addEventListener('error', function(e) {
    if (e.target.readyState != EventSource.OPEN) {
      console.log("Server Events Disconnected");
    }
  }, false);
}

// Utilities
function bit_test(num, bit){return ((num>>bit) % 2 !== 0)}
function pad (n) {return (n < 10) ? ("0" + n) : n;}

// Define pin numbers and name
var Pins = {  pinName:   ["ALARM_ON", "HORN_ON", "REED_IN", "TEST"],
              pinNumber: [ 13,        12,         5,        14    ]
           };

var timePause, timeHorn = 0;
var modal = document.getElementById('myModal');
var butReset = document.getElementById("butAlarmReset");
var span = document.getElementsByClassName("close")[0];
var butAlarmOn = document.getElementById('butAlarmOn');
var butAlarmOff = document.getElementById('butAlarmOff');
var butTimer = document.getElementById('butTimer');
var ws = null;
var PinOK = false;

function startSocket(){
  ws = new WebSocket('ws://'+document.location.host+'/ws');
  // Connection opened
  ws.addEventListener('open', function (e) {
      ws.send('Hello Server!');
      console.log("Connected with server " + "ws://"+document.location.host);
  });

  ws.addEventListener('close', function (e) {
    console.log("Disconnected from server " + "ws://"+document.location.host);
  });

  ws.addEventListener('error', function (e) {
    console.log("ws error", e);
    location.reload(); 
  });

  
  // Data from ESP
  ws.addEventListener('message', function (e) {
    var msg = e.data;
    try {
      //console.log(msg);
      obj = JSON.parse(msg);
      
      // Update time
      var dt = new Date(obj.ts*1000 + 3600000*-2); // offset
      var strDate = pad(dt.getHours()) + ":" + pad(dt.getMinutes()) + ":" +pad(dt.getSeconds()) ;
      document.getElementById("serverDate").innerHTML = "Smart Alarm v1.0  &emsp;&emsp;&emsp;&emsp;&emsp;" + strDate ;
      
      var states = { DISABLED:0, ENABLED:10, RUNNING:11, WAIT_NODE:12, CHECK_NODES:13, PAUSED:20, TIMED:30, ALARMED:99 } ;
      
      // Update system status
      if (obj.cmd == "status") {
        var status = parseInt(obj.msg);
        if(status === states.DISABLED){
          butTimer.disabled = false;
          butAlarmOff.disabled = true;
          butAlarmOn.disabled = false;
          console.log( "Alarm system is DISABLED");
        }
        if(status >= states.ENABLED && status < states.PAUSED){
          butTimer.disabled = false;
          butAlarmOff.disabled = false;
          butAlarmOn.disabled = true;
          console.log( "Alarm system is ENABLED");
        }
        if(status == states.PAUSED){
          butTimer.disabled = false;
          butAlarmOff.disabled = false;
          butAlarmOn.disabled = true;
          console.log( "Alarm system is going to be ENABLED");
        }
        if(status >= states.TIMED && status < states.ALARMED){
          butTimer.disabled = true;
          butAlarmOff.disabled = false;
          butAlarmOn.disabled = false;
          console.log( "Alarm system is in daily TIMER MODE");
        }
        if(status == states.ALARMED){
          alarmActivated();
          console.log( "Alarm system ALARM!");
        }
      
        // Update sensor status and battery values
        var nodes = obj.nodeid;
        var stat  = obj.state_battery;
        for(i=1; i<nodes.length; i++){
          var nodeElement = document.getElementById("sensor-" + (i));
          if(nodes[i] !== 0){
            var battery = stat[i];
            nodeElement.style.background = "#99cc00";
            if(battery > 100){
              battery = battery - 100;
              nodeElement.innerHTML = (battery/10).toFixed(2) + "V";
              if(battery < 34)
                nodeElement.style.background = "#ff9933";
              if (battery < 32)
                nodeElement.style.background = "#ff3300";
            }
            else 
              nodeElement.style.background = "#909090";
          } 
          else {
              nodeElement.innerHTML = "OFF";
              nodeElement.style.background = "#f3f3f3";
            }
        }
        // Update GPIO
        var gpio = obj.gpio;
        // estrapolate single I/O values
        for (i = 0; i < 16; i++) {
          try {
            // Get the element and change the class name according to the status of I/O pin
            var element =  document.getElementById(Pins.pinName[i]);
            // if element exists.
            if (typeof(element) != 'undefined' && element !== null){
              var clName = element.className;
              if(bit_test(gpio, Pins.pinNumber[i]))
                element.className = clName.replace("-on", "-off");
              else
                element.className = clName.replace("-off", "-on");
            }
          }
          catch(err) {console.log(err); }
        }
      }
      
      // Result from hash request
      if (obj.cmd == "checkThisHash") {
        var result = obj.msg;
        console.log("Hash: " + result);
        if(result === "true")
          PinOK = true;
        else
          PinOK = false;
      }
      
      // set global timers
      if (obj.cmd == "jsVars") {
        timePause = obj.timePause;
        timeHorn = obj.timeHorn;
        console.log("Tempo sirena: " + timeHorn +" secondi");
        console.log("Tempo pausa: " + timePause +" secondi");
      }
      
    }
    catch (e) {console.log(e)}
  });
}

var alarmActive = false;

function noAlarm(){
  document.getElementById('butAlarmReset').disabled = true;
  document.getElementById('alarmBox').style.display = "none";
  document.getElementById('normal').style.display = "block";
  alarmActive = false;
}

function alarmActivated(){
  document.getElementById('butAlarmReset').disabled = false;
  document.getElementById('alarmBox').style.display = "flex";
  document.getElementById('normal').style.display = "none";
  if(!alarmActive){
    var myBar = document.getElementById("bar");
    var myLabel = document.getElementById("labelProg");
    var current = 1;
    var timeout = parseInt(timeHorn);    
    var looper= setInterval(progressBar, 1000);
    function progressBar(){
      if(current >= timeout){
        myLabel.innerHTML = "Sirena Attiva!";
        clearInterval(looper);
      }else{
        current++;    
        myLabel.innerHTML = "Sirena attiva tra " + (timeout+1-current).toString() +" secondi!";
        myBar.style.width=current*6.67+"%";
      }
    }
  }
  alarmActive = true;
}

function toggle(div) {
  var id = div.id;
  var pin = -1;
  var state = 0;
  for (i = 0; i < 11; i++) {
    if(Pins.pinName[i] == id){
      var element =  document.getElementById(Pins.pinName[i]);
      if (typeof(element) != 'undefined' && element !== null)
        if(element.className.indexOf("-on") > 0)
          state = 1;
      pin = Pins.pinNumber[i];
    }
  }
  setOutput(id, pin, state);
}


function setOutput(_pinName, _pin, _state) {
  var datatosend = {};
  datatosend.command = "setOutput";
  datatosend.pinName = _pinName;
  datatosend.pin = _pin;
  datatosend.state = _state;
  console.log(JSON.stringify(datatosend));
  ws.send(JSON.stringify(datatosend));
}

span.onclick = function() {
  modal.style.display = "none";
  resetGrow();   
}
window.onclick = function(event) {
  if (event.target == modal) {
    modal.style.display = "none";
    resetGrow();
  }
}

butReset.onclick = function() {
  checkPin("statusChange", "reset");
}

butAlarmOff.onclick = function() {
  checkPin("statusChange", "disable");
}

butTimer.onclick = function() {
  checkPin("statusChange", "timer");
}

butAlarmOn.onclick = function() {
  var datatosend = {};
  datatosend.command = "alarmOn";
  console.log(JSON.stringify(datatosend));
  ws.send(JSON.stringify(datatosend));
}


function longPressEvent (e) {
  e.addEventListener('mousedown', () => {
    const whilePressing = setTimeout(() => {
      checkPin("sensorDisable", e.id);
    }, 1000) 

    e.addEventListener('mouseup', () => {
      clearTimeout(whilePressing);
    })
  })
}

function sensorToggleStatus(par){
  var sensorId = parseInt(par.replace("sensor-", ""));
  console.log("Toggle sensor " + sensorId + " status.");
  var datatosend = {};
  datatosend.command = "sensorToggle";
  datatosend.sensor = sensorId;
  console.log(JSON.stringify(datatosend));
  ws.send(JSON.stringify(datatosend));
  document.getElementById("sensor-" + sensorId).innerHTML = "OFF";
  document.getElementById("sensor-" + sensorId).style.background = "#f3f3f3";
}

function showDialog(){
  $( "#dialog-confirm" ).dialog({
    resizable: false,
    height: "auto",
    width: 400,
    modal: true,
    buttons: {
      "ATTIVA TIMER": function() {
        noAlarm();
        var datatosend = {};
        datatosend.command = "timer";
        console.log(JSON.stringify(datatosend));
        ws.send(JSON.stringify(datatosend));
        $( this ).dialog( "close" );
      },
      "RIATTIVA": function() {
        noAlarm();
        var datatosend = {};
        datatosend.command = "pause";
        console.log(JSON.stringify(datatosend));
        ws.send(JSON.stringify(datatosend));
        $( this ).dialog( "close" );
      },
      "DISATTIVA": function() {
        noAlarm();
        var datatosend = {};
        datatosend.command = "alarmOff";
        console.log(JSON.stringify(datatosend));
        ws.send(JSON.stringify(datatosend));
        $( this ).dialog( "close" );
      }
    }
  });
}

function resetGrow(){
  var numbers = document.getElementsByClassName('number');
  for(var i=0; i<numbers.length; i++)
    numbers[i].classList.remove("grow");
  document.body.className = '';
}

function checkPin(functionToDo, param1) {
  resetGrow();
  modal.style.display = "block";
  var input = '';
  var dots = document.getElementsByClassName('dot'),
      numbers = document.getElementsByClassName('number');
  dots = Array.from(dots);
  numbers = Array.from(numbers);
  var numbersBox = document.getElementsByClassName('numbers')[0];
  
  $(numbersBox).on('click', '.number', function (evt) {
    var number = $(this);
    number.addClass('grow');
    if(number.index()==10){
      input += 0;
    } else {
        input += number.index() + 1;
      }
    $(dots[input.length - 1]).addClass('active');

    if (input.length >= 5) {
      var hash = sha256.create();
      hash.update(input);
      var strHash = hash.hex();

      // Send hash request to server
      jsonData = JSON.stringify({'command':"checkThisHash", 'testHash':strHash.toUpperCase()});
      ws.send(jsonData);

      // After some ms check the result
      setTimeout(function(){
        if (!PinOK) {
          dots.forEach(function (dot) {return $(dot).addClass('wrong'); });
          $(document.body).addClass('wrong');
        } 
        else {
          dots.forEach(function (dot) {return $(dot).addClass('correct'); });
          $(document.body).addClass('correct');
          modal.style.display = "none";
          
          if(functionToDo == "statusChange")
            showDialog();
            
          if(functionToDo == "sensorDisable")
            var f = sensorToggleStatus(param1);// console.log("Disable sensor n° " + param1);
          
          functionToDo ="";
          param1 = "";
        }
      }, 300);

      setTimeout(function () {
        dots.forEach(function (dot) {
          return dot.className = 'dot';
        });
        input = '';
      }, 400);
    }
  });
}