import { eASICModel } from './enum/eASICModel';

export interface ISystemInfo {

    flipscreen: number;
    invertscreen: number;
    power: number,
    voltage: number,
    current: number,
    fanSpeed: number,
    temp: number,
    hashRate: number,
    bestDiff: string,
    freeHeap: number,
    coreVoltage: number,
    ssid: string,
    wifiStatus: string,
    sharesAccepted: number,
    sharesRejected: number,
    SessionID: string,
    uptimeSeconds: number,
    ASICModel: eASICModel,
    PCB_Rev: string,
    stratumURL: string,
    stratumPort: number,
    stratumUser: string,
    maxPower: number,
    maxFrequency: number,
    frequency: number,
    version: string,
    invertfanpolarity: number,
    autofanspeed: number,
    fanspeed: number,
    efficiency: number
    coreVoltageActual: number
}