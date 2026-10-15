import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

export default {
    zigbeeModel: ['nRF_moisture'],
    model: 'nRF_moisture',
    vendor: 'Marco Corp.',
    description: 'Automatically generated definition',
    extend: [
        m.deviceEndpoints({"endpoints":{"1":1,"2":2}}),
        m.battery(
            {voltage: true, voltageReporting: true, percentageReportingConfig:{"min": "1_HOUR", "max": "MAX", "change": 0}, voltageReportingConfig: {"min": "1_HOUR", "max": "MAX", "change": 0} }
        ),
        m.numeric(
            {"name":"sensor_voltage","label":"sensor_voltage","precision": 0,"valueMin":0,"valueMax":0,"valueStep":1,"cluster":"genAnalogInput","attribute":"presentValue","reporting":{"min":"MIN","max":"MAX","change":1},"description":"Moisture sensor voltage","access":"STATE_GET","endpointNames":["2"],"unit":"mV"}
        )
    ],
};